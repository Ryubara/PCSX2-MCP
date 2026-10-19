// SPDX-FileCopyrightText: 2026 PS2Recomp Debug Bridge
// SPDX-License-Identifier: MIT
//
// PCSX2 Debug Server — JSON/TCP API wrapping full DebugInterface
// Protocol: newline-delimited JSON over TCP (port 21512)
//
// Request:  {"cmd":"read_registers","cpu":"ee","category":0}\n
// Response: {"ok":true,"data":{...}}\n
//
// Integration:
//   1. Drop DebugServer.h + DebugServer.cpp into pcsx2/DebugTools/
//   2. Add to CMakeLists.txt
//   3. Call DebugServer::Start() from VMManager::Initialize()
//   4. Call DebugServer::Stop() from VMManager::Shutdown()
//   5. Call DebugServer::OnBreakpointHit() from breakpoint handler

// ============================================================
// NOTE: This file uses a minimal inline JSON builder to avoid
// external dependencies. PCSX2 doesn't bundle nlohmann/json
// in all configurations.
// ============================================================

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX // windows.h's min/max macros break std::numeric_limits<T>::max() in PCSX2's headers
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define SOCKET_INVALID INVALID_SOCKET
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int socket_t;
#define SOCKET_INVALID (-1)
#define CLOSE_SOCKET close
#endif

#include "DebugServer.h"
#include "DebugInterface.h"
#include "Breakpoints.h"
#include "MipsStackWalk.h"
#include "Config.h"
#include "MTGS.h"
#include "GS/GS.h"
#include "GS/GSCapture.h"
#include "GS/GSDump.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "common/FileSystem.h"
#include "common/Image.h"
#include "common/Path.h"
#include "Counters.h"
#include "Host.h"
#include "VMManager.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"
#include "Recording/InputRecording.h"

#include <cstring>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <future>
#include <functional>
#include <memory>
#include <tuple>

// Forward declarations — these are PCSX2 globals
extern R5900DebugInterface r5900Debug;
extern R3000DebugInterface r3000Debug;

namespace DebugServer
{
	// ============================================================
	// Minimal JSON Builder
	// ============================================================
	class JsonBuilder
	{
	public:
		void startObject() { comma(); m_buf += '{'; m_needComma.push_back(false); }
		void endObject() { m_buf += '}'; m_needComma.pop_back(); if (!m_needComma.empty()) m_needComma.back() = true; }
		void startArray() { comma(); m_buf += '['; m_needComma.push_back(false); }
		void endArray() { m_buf += ']'; m_needComma.pop_back(); if (!m_needComma.empty()) m_needComma.back() = true; }

		void key(const char* k)
		{
			comma();
			m_buf += '"';
			escapeStr(k);
			m_buf += "\":";
			m_needComma.back() = false; // value follows
		}

		void valStr(const char* v) { comma(); m_buf += '"'; escapeStr(v); m_buf += '"'; m_needComma.back() = true; }
		void valStr(const std::string& v) { valStr(v.c_str()); }
		void valInt(int64_t v) { comma(); m_buf += std::to_string(v); m_needComma.back() = true; }
		void valUint(uint64_t v) { comma(); m_buf += std::to_string(v); m_needComma.back() = true; }
		void valBool(bool v) { comma(); m_buf += v ? "true" : "false"; m_needComma.back() = true; }
		void valNull() { comma(); m_buf += "null"; m_needComma.back() = true; }

		void valHex32(uint32_t v)
		{
			char buf[16];
			snprintf(buf, sizeof(buf), "0x%08x", v);
			comma();
			m_buf += '"';
			m_buf += buf;
			m_buf += '"';
			m_needComma.back() = true;
		}

		void valHex128(u128 v)
		{
			char buf[40];
			snprintf(buf, sizeof(buf), "%08x%08x%08x%08x",
				v._u32[3], v._u32[2], v._u32[1], v._u32[0]);
			comma();
			m_buf += '"';
			m_buf += buf;
			m_buf += '"';
			m_needComma.back() = true;
		}

		// Key-value shortcuts
		void kv(const char* k, const char* v) { key(k); valStr(v); }
		void kv(const char* k, const std::string& v) { key(k); valStr(v); }
		void kv(const char* k, int v) { key(k); valInt(static_cast<int64_t>(v)); }
		void kv(const char* k, unsigned int v) { key(k); valUint(static_cast<uint64_t>(v)); }
		void kv(const char* k, int64_t v) { key(k); valInt(v); }
		void kv(const char* k, uint64_t v) { key(k); valUint(v); }
		void kv(const char* k, bool v) { key(k); valBool(v); }

		std::string str() const { return m_buf; }
		void clear() { m_buf.clear(); m_needComma.clear(); }

	private:
		void comma()
		{
			if (!m_needComma.empty() && m_needComma.back())
				m_buf += ',';
		}
		void escapeStr(const char* s)
		{
			for (; *s; ++s)
			{
				switch (*s)
				{
					case '"': m_buf += "\\\""; break;
					case '\\': m_buf += "\\\\"; break;
					case '\n': m_buf += "\\n"; break;
					case '\r': m_buf += "\\r"; break;
					case '\t': m_buf += "\\t"; break;
					default: m_buf += *s; break;
				}
			}
		}

		std::string m_buf;
		std::vector<bool> m_needComma;
	};

	// ============================================================
	// Minimal JSON Parser (just enough for our commands)
	// ============================================================
	struct JsonValue
	{
		enum Type { NONE, STRING, NUMBER, BOOL, OBJECT };
		Type type = NONE;
		std::string strVal;
		int64_t numVal = 0;
		bool boolVal = false;
	};

	static std::unordered_map<std::string, JsonValue> parseJsonObject(const std::string& json)
	{
		std::unordered_map<std::string, JsonValue> result;
		size_t i = 0;
		// Skip to first '{'
		while (i < json.size() && json[i] != '{') i++;
		i++; // skip '{'

		while (i < json.size())
		{
			// Skip whitespace/commas
			while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r' || json[i] == ','))
				i++;
			if (i >= json.size() || json[i] == '}') break;

			// Read key
			if (json[i] != '"') break;
			i++;
			std::string key;
			while (i < json.size() && json[i] != '"')
			{
				if (json[i] == '\\' && i + 1 < json.size()) { key += json[i + 1]; i += 2; }
				else { key += json[i]; i++; }
			}
			i++; // skip closing '"'

			// Skip colon
			while (i < json.size() && json[i] != ':') i++;
			i++;

			// Skip whitespace
			while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) i++;

			JsonValue val;
			if (json[i] == '"')
			{
				// String value
				i++;
				std::string sv;
				while (i < json.size() && json[i] != '"')
				{
					if (json[i] == '\\' && i + 1 < json.size()) { sv += json[i + 1]; i += 2; }
					else { sv += json[i]; i++; }
				}
				i++; // skip closing '"'
				val.type = JsonValue::STRING;
				val.strVal = sv;
			}
			else if (json[i] == 't' || json[i] == 'f')
			{
				val.type = JsonValue::BOOL;
				val.boolVal = (json[i] == 't');
				while (i < json.size() && json[i] != ',' && json[i] != '}') i++;
			}
			else if (json[i] == '-' || (json[i] >= '0' && json[i] <= '9'))
			{
				std::string ns;
				bool isHex = false;
				if (json[i] == '0' && i + 1 < json.size() && (json[i + 1] == 'x' || json[i + 1] == 'X'))
				{
					isHex = true;
					i += 2;
				}
				while (i < json.size() && ((json[i] >= '0' && json[i] <= '9') ||
					   json[i] == '-' ||
					   (isHex && ((json[i] >= 'a' && json[i] <= 'f') || (json[i] >= 'A' && json[i] <= 'F')))))
				{
					ns += json[i]; i++;
				}
				val.type = JsonValue::NUMBER;
				if (isHex) val.numVal = (int64_t)strtoull(ns.c_str(), nullptr, 16);
				else val.numVal = strtoll(ns.c_str(), nullptr, 10);
			}
			else
			{
				// Skip unknown
				while (i < json.size() && json[i] != ',' && json[i] != '}') i++;
			}

			result[key] = val;
		}

		return result;
	}

	static std::string getStr(const std::unordered_map<std::string, JsonValue>& m, const char* key, const char* def = "")
	{
		auto it = m.find(key);
		if (it != m.end() && it->second.type == JsonValue::STRING) return it->second.strVal;
		return def;
	}

	static int64_t getNum(const std::unordered_map<std::string, JsonValue>& m, const char* key, int64_t def = 0)
	{
		auto it = m.find(key);
		if (it != m.end() && it->second.type == JsonValue::NUMBER) return it->second.numVal;
		// Also try parsing string as hex
		if (it != m.end() && it->second.type == JsonValue::STRING)
		{
			const auto& s = it->second.strVal;
			if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
				return (int64_t)strtoull(s.c_str() + 2, nullptr, 16);
			return strtoll(s.c_str(), nullptr, 10);
		}
		return def;
	}

	static bool getBool(const std::unordered_map<std::string, JsonValue>& m, const char* key, bool def = false)
	{
		auto it = m.find(key);
		if (it != m.end() && it->second.type == JsonValue::BOOL) return it->second.boolVal;
		return def;
	}

	// ============================================================
	// Get DebugInterface by CPU name
	// ============================================================
	static DebugInterface* getCpu(const std::string& name)
	{
		if (name == "iop" || name == "r3000" || name == "IOP")
			return &r3000Debug;
		return &r5900Debug; // default to EE
	}

	static BreakPointCpu getBpCpu(const std::string& name)
	{
		if (name == "iop" || name == "r3000" || name == "IOP")
			return BREAKPOINT_IOP;
		return BREAKPOINT_EE;
	}

	// ============================================================
	// Emulator thread helpers (controller, frame advance, emulation)
	// ============================================================

	// Runs fn on PCSX2's CPU thread, which also answers while emulation is paused; false when it does not answer
	// within the timeout.
	static bool runOnCpuThread(std::function<void()> fn, std::chrono::milliseconds timeout = std::chrono::seconds(5))
	{
		auto done = std::make_shared<std::promise<void>>();
		auto got = done->get_future();
		Host::RunOnCPUThread([fn = std::move(fn), done]() {
			fn();
			done->set_value();
		});
		return got.wait_for(timeout) == std::future_status::ready;
	}

	static const char* vmStateName(VMState state)
	{
		switch (state)
		{
			case VMState::Running: return "running";
			case VMState::Paused: return "paused";
			case VMState::Initializing: return "initializing";
			case VMState::Stopping: return "stopping";
			default: return "shutdown";
		}
	}

	// The DualShock 2 buttons by name: pressure buttons take SetRawPressureButton, the rest Set (as input recordings
	// apply them).
	struct PadButton { const char* name; u32 input; bool pressure; };
	static constexpr PadButton s_padButtons[] = {
		{"up", PadDualshock2::Inputs::PAD_UP, true}, {"right", PadDualshock2::Inputs::PAD_RIGHT, true},
		{"down", PadDualshock2::Inputs::PAD_DOWN, true}, {"left", PadDualshock2::Inputs::PAD_LEFT, true},
		{"triangle", PadDualshock2::Inputs::PAD_TRIANGLE, true}, {"circle", PadDualshock2::Inputs::PAD_CIRCLE, true},
		{"cross", PadDualshock2::Inputs::PAD_CROSS, true}, {"square", PadDualshock2::Inputs::PAD_SQUARE, true},
		{"l1", PadDualshock2::Inputs::PAD_L1, true}, {"l2", PadDualshock2::Inputs::PAD_L2, true},
		{"r1", PadDualshock2::Inputs::PAD_R1, true}, {"r2", PadDualshock2::Inputs::PAD_R2, true},
		{"select", PadDualshock2::Inputs::PAD_SELECT, false}, {"start", PadDualshock2::Inputs::PAD_START, false},
		{"l3", PadDualshock2::Inputs::PAD_L3, false}, {"r3", PadDualshock2::Inputs::PAD_R3, false},
	};

	static bool runOnGsThread(std::function<void()> fn, std::chrono::milliseconds timeout = std::chrono::seconds(5))
	{
		auto done = std::make_shared<std::promise<void>>();
		auto got = done->get_future();
		MTGS::RunOnGSThread([fn = std::move(fn), done]() {
			fn();
			done->set_value();
		});
		return got.wait_for(timeout) == std::future_status::ready;
	}

	// A plain file name: letters, digits, '.', '_' and '-', up to 100, not "." or ".."
	static bool plainFileName(const std::string& name)
	{
		bool nameOk = name.size() <= 100;
		for (char c : name)
			nameOk = nameOk && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
									c == '.' || c == '_' || c == '-');
		return nameOk && name != "." && name != "..";
	}

	// The file name a command was given ("name": letters, digits, '.', '_' and '-', up to 100), or a stamped default
	// mcp-<date>-<time>-<ms>; "ext" is added when it does not end with it. Empty when the name is not allowed.
	static std::string fileName(const std::unordered_map<std::string, JsonValue>& params, const std::string& ext)
	{
		std::string name = getStr(params, "name", "");
		if (!plainFileName(name))
			return {};
		if (name.empty())
		{
			const auto now = std::chrono::system_clock::now();
			const std::time_t t = std::chrono::system_clock::to_time_t(now);
			const int ms = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
			char stamp[32];
			std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&t));
			char buf[48];
			snprintf(buf, sizeof(buf), "mcp-%s-%03d", stamp, ms);
			name = buf;
		}
		if (name.size() < ext.size() || name.compare(name.size() - ext.size(), ext.size(), ext) != 0)
			name += ext;
		return name;
	}

	static const char* const FILE_NAME_RULE = "name must be a file name of letters, digits, '.', '_' and '-' (up to 100)";

	// Input recordings stay in this instance's own data folder (EmuFolders::DataRoot: a portable install's own
	// folder), at a relative path of plain names; a new one goes into its recordings folder by default. PCSX2 keeps a
	// recording's start savestate beside it, as <file>.p2m2_SaveState.p2s.
	static constexpr const char* INPUT_RECORDING_DIR = "recordings";
	static constexpr std::string_view INPUT_RECORDING_EXT = ".p2m2";
	static constexpr size_t INPUT_RECORDING_MAX_PARTS = 8;
	static const char* const RECORDING_PATH_RULE = "path must be relative to PCSX2's data folder: up to 8 names of "
												   "letters, digits, '.', '_' and '-' (up to 100 each) split by '/', "
												   "with no '.' or '..' (.p2m2 is added when missing)";

	// The full path of a recording given relative to the data folder, or empty when it is not allowed.
	static std::string recordingPath(std::string given)
	{
		if (!given.ends_with(INPUT_RECORDING_EXT))
			given += INPUT_RECORDING_EXT;
		std::vector<std::string> parts(1);
		for (char c : given)
		{
			if (c == '/' || c == '\\')
				parts.emplace_back();
			else
				parts.back() += c;
		}
		if (parts.size() > INPUT_RECORDING_MAX_PARTS)
			return {};
		std::string path = EmuFolders::DataRoot;
		for (const std::string& part : parts)
		{
			if (part.empty() || !plainFileName(part))
				return {};
			path = Path::Combine(path, part);
		}
		return path;
	}

	// Once a recording's folder exists: whether that real folder lies inside the real data folder (no link leads out).
	static bool insideDataRoot(const std::string& path)
	{
		const std::string root = Path::RealPath(EmuFolders::DataRoot);
		const std::string dir = Path::RealPath(Path::GetDirectory(path));
		if (root.empty() || dir.compare(0, root.size(), root) != 0)
			return false;
		return dir.size() == root.size() || dir[root.size()] == '/' || dir[root.size()] == '\\';
	}

	// A replay this server started and closes at its last frame (EndInputRecordingReplay), and the last one it closed
	// that way. Used on the CPU thread only, like PCSX2's input recording itself.
	struct EndedReplay
	{
		std::string path;
		u32 frame = 0, totalFrames = 0; // the recording's frame it ended at, and its total frames
		u32 endedAt = 0;                // g_FrameCount then
	};
	static bool s_replayStopsAtEnd = false;
	static std::string s_replayPath;
	static bool s_replayEnded = false;
	static EndedReplay s_endedReplay;

	// What PCSX2's input recording holds now; read on the CPU thread, which alone drives it.
	struct InputRecordingState
	{
		bool active = false;
		bool recording = false;
		bool fromSavestate = false;
		std::string path, author, game;
		// While none is active, frame and totalFrames are the last recording's: PCSX2 keeps them after closing it
		u32 frame = 0, totalFrames = 0, undoCount = 0, startingFrame = 0;
		bool replayEnded = false; // the last replay this server started ended at its last frame and was closed
		EndedReplay ended;
	};

	static InputRecordingState readInputRecording()
	{
		InputRecordingState s;
		s.active = g_InputRecording.isActive();
		const InputRecordingFile& file = g_InputRecording.getData();
		s.frame = g_InputRecording.getFrameCounter();
		s.totalFrames = file.getTotalFrames();
		s.replayEnded = s_replayEnded;
		if (s_replayEnded)
			s.ended = s_endedReplay;
		if (!s.active)
			return s;
		s.recording = g_InputRecording.getControls().isRecording();
		s.fromSavestate = g_InputRecording.isTypeSavestate();
		s.path = file.getFilename();
		s.author = file.getAuthor();
		s.game = file.getGameName();
		s.undoCount = file.getUndoCount();
		s.startingFrame = g_InputRecording.getStartingFrame();
		return s;
	}

	// The file the running capture writes to, as this server started it ("" when none did).
	static std::mutex s_captureLock;
	static std::string s_capturePath;

	// The last GS dump this server queued ("path" empty when none). PCSX2 starts a queued dump on the GS thread's
	// next vsync and ends it once its frames have passed, so nothing moves while emulation is paused.
	struct GsDump
	{
		std::string path;       // the dump file: the base name and the GSDumpCompression setting's extension
		std::string screenshot; // the screenshot PCSX2 saves beside it
		u32 frames = 0;
		u32 queuedAt = 0;       // g_FrameCount when it was queued
	};
	static std::mutex s_gsDumpLock;
	static GsDump s_gsDump;
	static constexpr int64_t GS_DUMP_MAX_FRAMES = 300;

	// Where a dump stands; called on the GS thread, which alone touches the renderer's dump. "dropped": PCSX2 did not
	// take it (a screenshot or dump it had queued itself came first) or the GS thread closed before it began.
	static const char* gsDumpState(const GsDump& d)
	{
		if (g_gs_renderer && g_gs_renderer->m_dump && g_gs_renderer->m_dump->GetPath() == d.path)
			return "recording";
		if (FileSystem::FileExists(d.path.c_str()))
			return "done";
		if (!g_gs_renderer || g_FrameCount >= d.queuedAt + 10)
			return "dropped";
		return "queued";
	}

	static std::string errorJson(const std::string& message)
	{
		JsonBuilder e;
		e.startObject();
		e.kv("ok", false);
		e.kv("error", message);
		e.endObject();
		return e.str();
	}

	// ============================================================
	// Command Handlers
	// ============================================================

	static std::string handleCommand(const std::string& jsonLine)
	{
		auto params = parseJsonObject(jsonLine);
		std::string cmd = getStr(params, "cmd");
		std::string cpuName = getStr(params, "cpu", "ee");
		DebugInterface* cpu = getCpu(cpuName);

		JsonBuilder j;

		// ----- STATUS -----
		if (cmd == "status")
		{
			j.startObject();
			j.kv("ok", true);
			j.key("data"); j.startObject();
			j.kv("alive", cpu->isAlive());
			j.kv("paused", cpu->isCpuPaused());
			j.key("pc"); j.valHex32(cpu->getPC());
			j.kv("cycles", (int64_t)cpu->getCycles());
			j.endObject();
			j.endObject();
		}
		// ----- READ REGISTERS (ALL) -----
		else if (cmd == "read_registers")
		{
			int cat = (int)getNum(params, "category", -1);
			j.startObject();
			j.kv("ok", true);
			j.key("data"); j.startObject();

			int catStart = (cat >= 0) ? cat : 0;
			int catEnd = (cat >= 0) ? cat + 1 : cpu->getRegisterCategoryCount();

			for (int c = catStart; c < catEnd; c++)
			{
				j.key(cpu->getRegisterCategoryName(c));
				j.startObject();
				j.kv("size", cpu->getRegisterSize(c));
				j.kv("count", cpu->getRegisterCount(c));
				j.key("regs"); j.startArray();
				for (int r = 0; r < cpu->getRegisterCount(c); r++)
				{
					j.startObject();
					j.kv("name", cpu->getRegisterName(c, r));
					j.key("value"); j.valHex128(cpu->getRegister(c, r));
					j.kv("display", cpu->getRegisterString(c, r));
					j.endObject();
				}
				j.endArray();
				j.endObject();
			}

			j.key("pc"); j.valHex32(cpu->getPC());
			j.key("hi"); j.valHex128(cpu->getHI());
			j.key("lo"); j.valHex128(cpu->getLO());
			j.endObject();
			j.endObject();
		}
		// ----- WRITE REGISTER -----
		else if (cmd == "write_register")
		{
			int cat = (int)getNum(params, "category", 0);
			int num = (int)getNum(params, "index", 0);
			// Value can be a hex string or number
			std::string valStr = getStr(params, "value", "0");
			u128 newVal = {};
			// Parse hex string (up to 128-bit)
			if (valStr.size() > 2 && valStr[0] == '0' && (valStr[1] == 'x' || valStr[1] == 'X'))
				valStr = valStr.substr(2);
			// Pad to 32 hex chars (128 bits)
			while (valStr.size() < 32) valStr = "0" + valStr;
			for (int i = 0; i < 4; i++)
			{
				std::string part = valStr.substr(24 - i * 8, 8);
				newVal._u32[i] = (uint32_t)strtoul(part.c_str(), nullptr, 16);
			}
			cpu->setRegister(cat, num, newVal);

			j.startObject();
			j.kv("ok", true);
			j.endObject();
		}
		// ----- SET PC -----
		else if (cmd == "set_pc")
		{
			u32 pc = (u32)getNum(params, "value", 0);
			cpu->setPc(pc);
			j.startObject();
			j.kv("ok", true);
			j.key("pc"); j.valHex32(pc);
			j.endObject();
		}
		// ----- READ MEMORY -----
		else if (cmd == "read_memory")
		{
			u32 addr = (u32)getNum(params, "address", 0);
			int len = (int)getNum(params, "length", 256);
			if (len > 65536) len = 65536;

			j.startObject();
			j.kv("ok", true);
			j.key("address"); j.valHex32(addr);
			j.kv("length", (int64_t)len);

			// Hex string output
			j.key("hex");
			std::string hexStr;
			hexStr.reserve(len * 2);
			for (int i = 0; i < len; i++)
			{
				bool valid = true;
				u32 byte = cpu->Read8(addr + i, &valid);
				if (!valid) byte = 0;
				char hb[4];
				snprintf(hb, sizeof(hb), "%02x", byte & 0xFF);
				hexStr += hb;
			}
			j.valStr(hexStr);
			j.endObject();
		}
		// ----- WRITE MEMORY -----
		else if (cmd == "write_memory")
		{
			u32 addr = (u32)getNum(params, "address", 0);
			std::string hexData = getStr(params, "data", "");
			int written = 0;
			for (size_t i = 0; i + 1 < hexData.size(); i += 2)
			{
				u8 byte = (u8)strtoul(hexData.substr(i, 2).c_str(), nullptr, 16);
				cpu->Write8(addr + written, byte);
				written++;
			}
			j.startObject();
			j.kv("ok", true);
			j.kv("written", (int64_t)written);
			j.endObject();
		}
		// ----- DISASSEMBLE -----
		else if (cmd == "disassemble")
		{
			u32 addr = (u32)getNum(params, "address", 0);
			int count = (int)getNum(params, "count", 20);
			bool simplify = getBool(params, "simplify", true);
			if (count > 500) count = 500;

			j.startObject();
			j.kv("ok", true);
			j.key("instructions"); j.startArray();
			for (int i = 0; i < count; i++)
			{
				u32 pc = addr + i * 4;
				if (!cpu->isValidAddress(pc)) break;

				bool valid = true;
				u32 opcode = cpu->Read32(pc, &valid);

				j.startObject();
				j.key("address"); j.valHex32(pc);
				j.key("opcode"); j.valHex32(opcode);
				j.kv("disasm", cpu->disasm(pc, simplify));
				j.endObject();
			}
			j.endArray();
			j.endObject();
		}
		// ----- EVALUATE EXPRESSION -----
		else if (cmd == "evaluate")
		{
			std::string expr = getStr(params, "expression", "0");
			u64 result = 0;
			std::string error;
			bool ok = cpu->evaluateExpression(expr.c_str(), result, error);

			j.startObject();
			j.kv("ok", ok);
			if (ok)
			{
				j.key("result"); j.valUint(result);
				char hexBuf[20];
				snprintf(hexBuf, sizeof(hexBuf), "0x%llx", (unsigned long long)result);
				j.kv("hex", hexBuf);
			}
			else
			{
				j.kv("error", error);
			}
			j.endObject();
		}
		// ----- SET BREAKPOINT -----
		else if (cmd == "set_breakpoint")
		{
			u32 addr = (u32)getNum(params, "address", 0);
			bool temp = getBool(params, "temporary", false);
			bool enabled = getBool(params, "enabled", true);
			std::string condExpr = getStr(params, "condition", "");
			std::string desc = getStr(params, "description", "");
			auto bpCpu = getBpCpu(cpuName);

			CBreakPoints::AddBreakPoint(bpCpu, addr, temp, enabled);

			if (!desc.empty())
				CBreakPoints::ChangeBreakPointDescription(bpCpu, addr, desc);

			if (!condExpr.empty())
			{
				BreakPointCond cond;
				cond.debug = cpu;
				cond.expressionString = condExpr;
				std::string error;
				if (cpu->initExpression(condExpr.c_str(), cond.expression, error))
				{
					CBreakPoints::ChangeBreakPointAddCond(bpCpu, addr, cond);
				}
			}

			j.startObject();
			j.kv("ok", true);
			j.key("address"); j.valHex32(addr);
			j.endObject();
		}
		// ----- REMOVE BREAKPOINT -----
		else if (cmd == "remove_breakpoint")
		{
			u32 addr = (u32)getNum(params, "address", 0);
			CBreakPoints::RemoveBreakPoint(getBpCpu(cpuName), addr);
			j.startObject();
			j.kv("ok", true);
			j.endObject();
		}
		// ----- SET MEMCHECK (WATCHPOINT) -----
		else if (cmd == "set_memcheck")
		{
			u32 start = (u32)getNum(params, "address", 0);
			u32 end = (u32)getNum(params, "end", start + 4);
			std::string typeStr = getStr(params, "type", "write");
			std::string actionStr = getStr(params, "action", "break");
			std::string desc = getStr(params, "description", "");
			std::string condExpr = getStr(params, "condition", "");

			MemCheckCondition cond = MEMCHECK_WRITE;
			if (typeStr == "read") cond = MEMCHECK_READ;
			else if (typeStr == "readwrite" || typeStr == "access") cond = MEMCHECK_READWRITE;
			else if (typeStr == "onchange") cond = (MemCheckCondition)(MEMCHECK_WRITE | MEMCHECK_WRITE_ONCHANGE);

			MemCheckResult result = MEMCHECK_BREAK;
			if (actionStr == "log") result = MemCheckResult(0x01); // PCSX2 dropped MEMCHECK_LOG: a non-zero result that does not break still counts hits
			else if (actionStr == "both") result = MEMCHECK_BOTH;

			auto bpCpu = getBpCpu(cpuName);
			CBreakPoints::AddMemCheck(bpCpu, start, end, cond, result);

			if (!desc.empty())
				CBreakPoints::ChangeMemCheckDescription(bpCpu, start, end, desc);

			if (!condExpr.empty())
			{
				BreakPointCond bpCond;
				bpCond.debug = cpu;
				bpCond.expressionString = condExpr;
				std::string error;
				if (cpu->initExpression(condExpr.c_str(), bpCond.expression, error))
					CBreakPoints::ChangeMemCheckAddCond(bpCpu, start, end, bpCond);
			}

			j.startObject();
			j.kv("ok", true);
			j.key("start"); j.valHex32(start);
			j.key("end"); j.valHex32(end);
			j.endObject();
		}
		// ----- REMOVE MEMCHECK -----
		else if (cmd == "remove_memcheck")
		{
			u32 start = (u32)getNum(params, "address", 0);
			u32 end = (u32)getNum(params, "end", start + 4);
			CBreakPoints::RemoveMemCheck(getBpCpu(cpuName), start, end);
			j.startObject();
			j.kv("ok", true);
			j.endObject();
		}
		// ----- LIST BREAKPOINTS -----
		else if (cmd == "list_breakpoints")
		{
			auto bps = CBreakPoints::GetBreakpoints(getBpCpu(cpuName), true);
			j.startObject();
			j.kv("ok", true);
			j.key("breakpoints"); j.startArray();
			for (const auto& bp : bps)
			{
				j.startObject();
				j.key("address"); j.valHex32(bp.addr);
				j.kv("enabled", bp.enabled);
				j.kv("temporary", bp.temporary);
				j.kv("stepping", bp.stepping);
				j.kv("has_condition", bp.hasCond);
				if (bp.hasCond)
					j.kv("condition", bp.cond.expressionString);
				if (!bp.description.empty())
					j.kv("description", bp.description);
				j.endObject();
			}
			j.endArray();
			j.endObject();
		}
		// ----- LIST MEMCHECKS -----
		else if (cmd == "list_memchecks")
		{
			auto mcs = CBreakPoints::GetMemChecks(getBpCpu(cpuName));
			j.startObject();
			j.kv("ok", true);
			j.key("memchecks"); j.startArray();
			for (const auto& mc : mcs)
			{
				j.startObject();
				j.key("start"); j.valHex32(mc.start);
				j.key("end"); j.valHex32(mc.end);
				j.kv("hits", (int64_t)mc.totalHits);
				j.key("last_pc"); j.valHex32(mc.lastPC);
				j.key("last_addr"); j.valHex32(mc.lastAddr);
				if (!mc.description.empty())
					j.kv("description", mc.description);
				j.endObject();
			}
			j.endArray();
			j.endObject();
		}
		// ----- PAUSE -----
		else if (cmd == "pause")
		{
			cpu->pauseCpu();
			j.startObject();
			j.kv("ok", true);
			j.key("pc"); j.valHex32(cpu->getPC());
			j.endObject();
		}
		// ----- RESUME -----
		else if (cmd == "resume")
		{
			// Skip current BP if we're sitting on one (matches PCSX2 GUI behavior)
			CBreakPoints::SetSkipFirst(getBpCpu(cpuName), cpu->getPC());
			cpu->resumeCpu();
			j.startObject();
			j.kv("ok", true);
			j.endObject();
		}
		// ----- STEP -----
		else if (cmd == "step")
		{
			u32 pc = cpu->getPC();

			// Skip current BP if we're sitting on one (matches PCSX2 GUI behavior)
			CBreakPoints::SetSkipFirst(getBpCpu(cpuName), pc);

			u32 nextPc = pc + 4;
			CBreakPoints::AddBreakPoint(getBpCpu(cpuName), nextPc, true, true, true);
			cpu->resumeCpu();

			// Wait for it to hit (with timeout)
			int timeout = 5000; // ms
			while (!cpu->isCpuPaused() && timeout > 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				timeout--;
			}

			u32 newPc = cpu->getPC();
			bool inBios = (newPc < 0x00100000) || (newPc >= 0x80000000 && newPc < 0x80100000);

			j.startObject();
			j.kv("ok", true);
			j.key("old_pc"); j.valHex32(pc);
			j.key("new_pc"); j.valHex32(newPc);
			j.kv("disasm", cpu->disasm(newPc, true));
			j.kv("in_bios", inBios);

			bool valid = true;
			u32 opcode = cpu->Read32(newPc, &valid);
			j.key("opcode"); j.valHex32(opcode);
			j.endObject();
		}
		// ----- STEP OVER -----
		else if (cmd == "step_over")
		{
			u32 pc = cpu->getPC();

			// Skip current BP if we're sitting on one (matches PCSX2 GUI behavior)
			CBreakPoints::SetSkipFirst(getBpCpu(cpuName), pc);

			bool valid = true;
			u32 opcode = cpu->Read32(pc, &valid);
			u32 op = (opcode >> 26) & 63;

			u32 bpAddr = pc + 8; // default: skip instruction + delay slot
			if (op == 3 || // JAL
				(op == 0 && (opcode & 63) == 9)) // JALR
			{
				bpAddr = pc + 8;
			}
			else
			{
				bpAddr = pc + 4;
			}

			CBreakPoints::AddBreakPoint(getBpCpu(cpuName), bpAddr, true, true, true);
			cpu->resumeCpu();

			int timeout = 10000;
			while (!cpu->isCpuPaused() && timeout > 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				timeout--;
			}

			u32 newPc = cpu->getPC();
			bool inBios = (newPc < 0x00100000) || (newPc >= 0x80000000 && newPc < 0x80100000);

			j.startObject();
			j.kv("ok", true);
			j.key("old_pc"); j.valHex32(pc);
			j.key("new_pc"); j.valHex32(newPc);
			j.kv("disasm", cpu->disasm(newPc, true));
			j.kv("in_bios", inBios);
			j.endObject();
		}
		// ----- GET THREADS -----
		else if (cmd == "get_threads")
		{
			auto threads = cpu->GetThreadList();
			j.startObject();
			j.kv("ok", true);
			j.key("threads"); j.startArray();
			for (const auto& t : threads)
			{
				j.startObject();
				j.kv("id", (int64_t)t->TID());
				j.key("pc"); j.valHex32(t->PC());
				j.kv("status", (int64_t)(int)t->Status());
				j.kv("wait_type", (int64_t)(int)t->Wait());
				j.endObject();
			}
			j.endArray();
			j.endObject();
		}
		// ----- GET MODULES (IOP only) -----
		else if (cmd == "get_modules")
		{
			auto mods = cpu->GetModuleList();
			j.startObject();
			j.kv("ok", true);
			j.key("modules"); j.startArray();
			for (const auto& m : mods)
			{
				j.startObject();
				j.kv("name", m.name);
				j.kv("version", (int64_t)m.version);
				j.endObject();
			}
			j.endArray();
			j.endObject();
		}
		// ----- GET BACKTRACE -----
		else if (cmd == "get_backtrace")
		{
			int maxFrames = (int)getNum(params, "max_frames", 32);
			if (maxFrames > 128) maxFrames = 128;

			u32 pc = cpu->getPC();
			u32 ra = cpu->getRegister(0, 31); // $ra
			u32 sp = cpu->getRegister(0, 29); // $sp

			// Find the running thread to get its entry point
			u32 threadEntry = 0;
			auto threads = cpu->GetThreadList();
			for (const auto& t : threads)
			{
				if (t->Status() == ThreadStatus::THS_RUN)
				{
					threadEntry = t->EntryPoint();
					break;
				}
			}

			auto frames = MipsStackWalk::Walk(cpu, pc, ra, sp, threadEntry);

			j.startObject();
			j.kv("ok", true);
			j.kv("frame_count", (int64_t)(std::min)((int)frames.size(), maxFrames));
			j.key("frames"); j.startArray();
			int count = 0;
			for (const auto& f : frames)
			{
				if (count >= maxFrames) break;
				j.startObject();
				j.key("entry"); j.valHex32(f.entry);
				j.key("pc"); j.valHex32(f.pc);
				j.key("sp"); j.valHex32(f.sp);
				j.kv("stack_size", (int64_t)f.stackSize);
				j.kv("disasm", cpu->disasm(f.pc, true));
				j.endObject();
				count++;
			}
			j.endArray();
			j.endObject();
		}
		// ----- IS VALID ADDRESS -----
		else if (cmd == "is_valid_address")
		{
			u32 addr = (u32)getNum(params, "address", 0);
			j.startObject();
			j.kv("ok", true);
			j.kv("valid", cpu->isValidAddress(addr));
			j.endObject();
		}
		// ----- READ STRING -----
		else if (cmd == "read_string")
		{
			u32 addr = (u32)getNum(params, "address", 0);
			int maxLen = (int)getNum(params, "max_length", 256);
			if (maxLen > 4096) maxLen = 4096;

			std::string str;
			for (int i = 0; i < maxLen; i++)
			{
				bool valid = true;
				u32 byte = cpu->Read8(addr + i, &valid);
				if (!valid || byte == 0) break;
				str += (char)byte;
			}

			j.startObject();
			j.kv("ok", true);
			j.kv("string", str);
			j.kv("length", (int64_t)str.size());
			j.endObject();
		}
		// ----- CLEAR ALL BREAKPOINTS -----
		else if (cmd == "clear_breakpoints")
		{
			CBreakPoints::ClearAllBreakPoints();
			CBreakPoints::ClearAllMemChecks();
			j.startObject();
			j.kv("ok", true);
			j.endObject();
		}
		// ----- SCREENSHOT -----
		// Saves the picture PCSX2 shows now (paused or running) as a PNG in its snapshots folder and returns the
		// path. "name" (optional): a file name of letters, digits, '.', '_' and '-'; ".png" is added if missing.
		else if (cmd == "screenshot")
		{
			const std::string name = fileName(params, ".png");
			if (name.empty())
				return errorJson(FILE_NAME_RULE);

			if (!MTGS::IsOpen())
			{
				j.startObject();
				j.kv("ok", false);
				j.kv("error", "no game is running");
				j.endObject();
				return j.str();
			}

			// The frame is read on the GS thread; the PNG is encoded here, so the GS thread is not held up.
			struct Frame { bool ok = false; u32 width = 0; u32 height = 0; std::vector<u32> pixels; };
			auto frame = std::make_shared<std::promise<Frame>>();
			auto got = frame->get_future();
			MTGS::RunOnGSThread([frame]() {
				Frame f;
				f.ok = g_gs_renderer && g_gs_device && g_gs_device->GetCurrent() &&
					   g_gs_renderer->SaveSnapshotToMemory(0, 0, true, true, &f.width, &f.height, &f.pixels);
				frame->set_value(std::move(f));
			});
			if (got.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
			{
				j.startObject();
				j.kv("ok", false);
				j.kv("error", "the GS thread did not answer within 5 s");
				j.endObject();
				return j.str();
			}
			Frame f = got.get();
			const std::string path = Path::Combine(EmuFolders::Snapshots, name);
			bool saved = false;
			if (f.ok)
			{
				FileSystem::CreateDirectoryPath(EmuFolders::Snapshots.c_str(), false);
				RGBA8Image image;
				image.SetPixels(f.width, f.height, std::move(f.pixels));
				saved = image.SaveToFile(path.c_str());
			}
			j.startObject();
			j.kv("ok", saved);
			if (saved)
			{
				j.kv("path", path);
				j.kv("width", (int64_t)f.width);
				j.kv("height", (int64_t)f.height);
			}
			else
				j.kv("error", f.ok ? "could not write " + path : std::string("no picture to capture yet"));
			j.endObject();
		}
		// ----- CONTROLLER -----
		// Sets a DualShock 2's whole state: "buttons" (comma-separated names) held, every other button released,
		// sticks "lx" "ly" "rx" "ry" 0-255 (127 = centre). It stays until the next controller command or a change
		// from the host's own input. "port": 0 or 1.
		else if (cmd == "controller")
		{
			const int64_t port = getNum(params, "port", 0);
			if (port < 0 || port > 1)
				return errorJson("port must be 0 or 1");
			std::vector<bool> held(std::size(s_padButtons), false);
			const std::string list = getStr(params, "buttons", "");
			size_t start = 0;
			while (start <= list.size())
			{
				size_t end = list.find(',', start);
				if (end == std::string::npos) end = list.size();
				std::string name = list.substr(start, end - start);
				const size_t first = name.find_first_not_of(' ');
				name = (first == std::string::npos) ? std::string() : name.substr(first, name.find_last_not_of(' ') - first + 1);
				for (char& c : name) c = (char)std::tolower((unsigned char)c);
				if (!name.empty())
				{
					size_t i = 0;
					while (i < std::size(s_padButtons) && name != s_padButtons[i].name) i++;
					if (i == std::size(s_padButtons))
						return errorJson("unknown button " + name + ": up right down left triangle circle cross square l1 l2 r1 r2 select start l3 r3");
					held[i] = true;
				}
				start = end + 1;
			}
			std::array<u8, 4> sticks;
			const char* stickKeys[4] = {"lx", "ly", "rx", "ry"};
			for (int k = 0; k < 4; k++)
			{
				const int64_t v = getNum(params, stickKeys[k], 127);
				if (v < 0 || v > 255)
					return errorJson(std::string(stickKeys[k]) + " must be 0 to 255 (127 = centre)");
				sticks[k] = (u8)v;
			}
			if (!VMManager::HasValidVM())
				return errorJson("no game is running");

			auto problem = std::make_shared<std::string>();
			const bool answered = runOnCpuThread([problem, held, sticks, port]() {
				PadBase* pad = Pad::GetPad((u8)port, 0);
				if (!pad || pad->GetType() != Pad::ControllerType::DualShock2)
				{
					*problem = "port " + std::to_string(port) + " has no DualShock 2";
					return;
				}
				pad->SetRawAnalogs({sticks[0], sticks[1]}, {sticks[2], sticks[3]});
				for (size_t i = 0; i < std::size(s_padButtons); i++)
				{
					if (s_padButtons[i].pressure)
						pad->SetRawPressureButton(s_padButtons[i].input, {held[i], (u8)(held[i] ? 255 : 0)});
					else
						pad->Set(s_padButtons[i].input, held[i] ? 1.0f : 0.0f);
				}
			});
			if (!answered)
				return errorJson("the CPU thread did not answer within 5 s");
			if (!problem->empty())
				return errorJson(*problem);
			j.startObject();
			j.kv("ok", true);
			j.kv("frame", (int64_t)g_FrameCount);
			j.key("held"); j.startArray();
			for (size_t i = 0; i < std::size(s_padButtons); i++)
				if (held[i]) j.valStr(s_padButtons[i].name);
			j.endArray();
			j.endObject();
		}
		// ----- FRAME ADVANCE -----
		// Runs "frames" (1-600) frames, then pauses, and answers once paused. Emulation held by the debugger
		// (pause, a breakpoint) does not advance: resume it first.
		else if (cmd == "frame_advance")
		{
			const int64_t frames = getNum(params, "frames", 1);
			if (frames < 1 || frames > 600)
				return errorJson("frames must be 1 to 600");
			if (!VMManager::HasValidVM())
				return errorJson("no game is running");
			const u32 before = g_FrameCount;
			if (!runOnCpuThread([frames]() { VMManager::FrameAdvance((u32)frames); }))
				return errorJson("the CPU thread did not answer within 5 s");
			// Wait for the pause a frame advance ends with: at least 20 frames a second, plus 5 s.
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(frames * 50 + 5000);
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			while (VMManager::GetState() != VMState::Paused && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			if (VMManager::GetState() != VMState::Paused)
				return errorJson("emulation did not pause again in time (is the debugger holding it? resume it first)");
			j.startObject();
			j.kv("ok", true);
			j.kv("frames", frames);
			j.kv("frame_before", (int64_t)before);
			j.kv("frame", (int64_t)g_FrameCount);
			j.endObject();
		}
		// ----- EMULATION -----
		// "action": pause or resume emulation as a whole (not the debugger's CPU break), or status.
		else if (cmd == "emulation")
		{
			const std::string action = getStr(params, "action", "status");
			if (action != "pause" && action != "resume" && action != "status")
				return errorJson("action must be pause, resume or status");
			if (action != "status")
			{
				if (!VMManager::HasValidVM())
					return errorJson("no game is running");
				const bool pause = action == "pause";
				if (!runOnCpuThread([pause]() { VMManager::SetPaused(pause); }))
					return errorJson("the CPU thread did not answer within 5 s");
			}
			j.startObject();
			j.kv("ok", true);
			j.kv("state", vmStateName(VMManager::GetState()));
			j.kv("frame", (int64_t)g_FrameCount);
			j.endObject();
		}
		// ----- CAPTURE -----
		// "action": start PCSX2's own video capture (through FFmpeg; codec, size and whether sound is recorded come
		// from its settings) into its videos folder, stop it, or status. Start takes an optional "name" as screenshot
		// does; the container's extension (the CaptureContainer setting, mp4 by default) is added. Only frames PCSX2
		// presents are recorded: none while paused, one for each frame advanced.
		else if (cmd == "capture")
		{
			const std::string action = getStr(params, "action", "status");
			if (action != "start" && action != "stop" && action != "status")
				return errorJson("action must be start, stop or status");
			if (action == "start")
			{
				const std::string name = fileName(params, "." + EmuConfig.GS.CaptureContainer);
				if (name.empty())
					return errorJson(FILE_NAME_RULE);
				if (!MTGS::IsOpen())
					return errorJson("no game is running");
				const std::string path = Path::Combine(EmuFolders::Videos, name);
				FileSystem::CreateDirectoryPath(EmuFolders::Videos.c_str(), false);
				// 0 started, 1 a capture already runs, 2 PCSX2 could not start it (its log says why)
				auto result = std::make_shared<int>(2);
				if (!runOnGsThread([path, result]() {
						if (GSCapture::IsCapturing())
							*result = 1;
						else if (GSBeginCapture(path))
							*result = 0;
					}))
					return errorJson("the GS thread did not answer within 5 s");
				if (*result == 1)
					return errorJson("a capture is already running: stop it first");
				if (*result == 2)
					return errorJson("PCSX2 could not start the capture (see its log)");
				std::lock_guard<std::mutex> lock(s_captureLock);
				s_capturePath = path;
			}
			else if (action == "stop")
			{
				// Stopping waits for the encoder to write what it still holds.
				auto was = std::make_shared<bool>(false);
				if (!runOnGsThread([was]() {
						*was = GSCapture::IsCapturing();
						if (*was)
							GSEndCapture();
					}, std::chrono::seconds(30)))
					return errorJson("the GS thread did not finish the capture within 30 s");
				if (!*was)
					return errorJson("no capture is running");
			}
			struct State { bool capturing = false; bool video = false; bool audio = false; std::string elapsed; };
			auto state = std::make_shared<State>();
			if (!runOnGsThread([state]() {
					state->capturing = GSCapture::IsCapturing();
					if (state->capturing)
					{
						state->video = GSCapture::IsCapturingVideo();
						state->audio = GSCapture::IsCapturingAudio();
						state->elapsed = GSCapture::GetElapsedTime().c_str();
					}
				}))
				return errorJson("the GS thread did not answer within 5 s");
			// The file is named while it is written, and once more by the first answer after the capture ended
			std::string path;
			{
				std::lock_guard<std::mutex> lock(s_captureLock);
				path = s_capturePath;
				if (!state->capturing)
					s_capturePath.clear();
			}
			j.startObject();
			j.kv("ok", true);
			j.kv("capturing", state->capturing);
			if (!path.empty())
				j.kv("path", path);
			if (state->capturing)
			{
				j.kv("video", state->video);
				j.kv("audio", state->audio);
				j.kv("elapsed", state->elapsed);
			}
			j.kv("frame", (int64_t)g_FrameCount);
			j.endObject();
		}
		// ----- GS DUMP -----
		// "action": start PCSX2's own GS dump (the .gs file its "Save Single/Multi Frame GS Dump" hotkeys make, with
		// the GSDumpCompression setting's extension and PCSX2's screenshot beside it) for "frames" (1-300, default 1),
		// or status of the last one. Start takes an optional "name" (as screenshot's, without extension; default
		// PCSX2's own <title>_<serial>_<time>) in the snapshots folder. With "wait" (default true) and emulation
		// running, start answers once the dump is written; paused, it stays queued until frames run.
		else if (cmd == "gs_dump")
		{
			const std::string action = getStr(params, "action", "start");
			if (action != "start" && action != "status")
				return errorJson("action must be start or status");
			const int64_t frames = getNum(params, "frames", 1);
			if (action == "start")
			{
				if (frames < 1 || frames > GS_DUMP_MAX_FRAMES)
					return errorJson("frames must be 1 to " + std::to_string(GS_DUMP_MAX_FRAMES));
				std::string name;
				if (!getStr(params, "name", "").empty())
				{
					name = fileName(params, "");
					if (name.empty())
						return errorJson(FILE_NAME_RULE);
				}
				if (!MTGS::IsOpen())
					return errorJson("no game is running");
				{
					std::lock_guard<std::mutex> lock(s_gsDumpLock);
					if (!s_gsDump.path.empty())
					{
						auto state = std::make_shared<std::string>("dropped");
						const GsDump last = s_gsDump;
						if (!runOnGsThread([state, last]() { *state = gsDumpState(last); }))
							return errorJson("the GS thread did not answer within 5 s");
						if (*state == "queued" || *state == "recording")
							return errorJson("the last GS dump is still " + *state + ": let it finish (action status)");
					}
				}
				// 0 queued, 1 PCSX2 is writing a dump of its own, 2 the file exists, 3 no renderer
				struct Queued { int result = 3; GsDump dump; };
				auto queued = std::make_shared<Queued>();
				if (!runOnGsThread([queued, name, frames]() {
						if (!g_gs_renderer)
							return;
						if (g_gs_renderer->m_dump)
						{
							queued->result = 1;
							return;
						}
						FileSystem::CreateDirectoryPath(EmuFolders::Snapshots.c_str(), false);
						const std::string base = name.empty() ? GSGetBaseSnapshotFilename() : Path::Combine(EmuFolders::Snapshots, name);
						const char* ext = GSConfig.GSDumpCompression == GSDumpCompressionMethod::Uncompressed ? ".gs" :
										  GSConfig.GSDumpCompression == GSDumpCompressionMethod::LZMA         ? ".gs.xz" :
																												".gs.zst";
						static constexpr const char* shotExt[] = {".png", ".jpg", ".webp"};
						const u8 format = static_cast<u8>(GSConfig.ScreenshotFormat);
						GsDump& d = queued->dump;
						d.path = base + ext;
						d.screenshot = base + (format < std::size(shotExt) ? shotExt[format] : ".png");
						d.frames = (u32)frames;
						d.queuedAt = g_FrameCount;
						if (FileSystem::FileExists(d.path.c_str()))
						{
							queued->result = 2;
							return;
						}
						// What the hotkeys call; a path ending in .png names the dump's base
						GSQueueSnapshot(base + ".png", (u32)frames);
						queued->result = 0;
					}))
					return errorJson("the GS thread did not answer within 5 s");
				if (queued->result == 1)
					return errorJson("PCSX2 is writing a GS dump of its own: let it finish first");
				if (queued->result == 2)
					return errorJson("the file exists already: " + queued->dump.path);
				if (queued->result == 3)
					return errorJson("no game is running");
				std::lock_guard<std::mutex> lock(s_gsDumpLock);
				s_gsDump = queued->dump;
			}

			GsDump dump;
			{
				std::lock_guard<std::mutex> lock(s_gsDumpLock);
				dump = s_gsDump;
			}
			if (dump.path.empty())
			{
				j.startObject();
				j.kv("ok", true);
				j.kv("state", "none");
				j.kv("frame", (int64_t)g_FrameCount);
				j.endObject();
				return j.str();
			}
			// Once the GS thread is gone, the file alone tells
			auto state = std::make_shared<std::string>(FileSystem::FileExists(dump.path.c_str()) ? "done" : "dropped");
			const auto readState = [&]() {
				return !MTGS::IsOpen() || runOnGsThread([state, dump]() { *state = gsDumpState(dump); });
			};
			if (!readState())
				return errorJson("the GS thread did not answer within 5 s");
			// Waiting is worth it only while frames run: at least 20 a second, plus 5 s
			if (action == "start" && getBool(params, "wait", true))
			{
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(dump.frames * 50 + 5000);
				while ((*state == "queued" || *state == "recording") && VMManager::GetState() == VMState::Running &&
					   std::chrono::steady_clock::now() < deadline)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
					if (!readState())
						return errorJson("the GS thread did not answer within 5 s");
				}
			}
			j.startObject();
			j.kv("ok", true);
			j.kv("state", *state);
			j.kv("path", dump.path);
			j.kv("screenshot", dump.screenshot);
			j.kv("frames", (int64_t)dump.frames);
			j.kv("emulation", vmStateName(VMManager::GetState()));
			j.kv("frame", (int64_t)g_FrameCount);
			j.endObject();
		}
		// ----- INPUT RECORDING -----
		// "action": PCSX2's own input recording (.p2m2), driven through the calls its "Input Recording" menu makes, on
		// the CPU thread. new: record into "path" (relative to the data folder; default recordings/mcp-<stamp>.p2m2)
		// with "author" (required, as the menu's dialog requires it), "from" savestate (the current point: PCSX2 saves
		// <file>_SaveState.p2s beside the recording, backing up one already there as .bak) or power_on (PCSX2 resets
		// the game and records from boot); an existing recording is refused unless "overwrite". play: replay "path",
		// which loads its start savestate or resets the game as the file says; at its last frame PCSX2 pauses and
		// EndInputRecordingReplay closes it, where PCSX2 alone would record on. stop: close it (PCSX2 closes it at
		// the end of the current frame while running) and answer its total frames and the frame it stopped at.
		// status: what is active, or the replay that ended.
		else if (cmd == "input_recording")
		{
			const std::string action = getStr(params, "action", "status");
			if (action != "new" && action != "play" && action != "stop" && action != "status")
				return errorJson("action must be new, play, stop or status");
			auto now = std::make_shared<InputRecordingState>();
			const auto readNow = [now]() { *now = readInputRecording(); };
			if (action == "new" || action == "play")
			{
				std::string given = getStr(params, "path", "");
				if (given.empty())
				{
					if (action == "play")
						return errorJson("play needs the recording's path");
					given = std::string(INPUT_RECORDING_DIR) + "/" + fileName({}, std::string(INPUT_RECORDING_EXT));
				}
				const std::string path = recordingPath(given);
				if (path.empty())
					return errorJson(RECORDING_PATH_RULE);
				const std::string from = getStr(params, "from", "savestate");
				const std::string author = getStr(params, "author", "");
				if (action == "new")
				{
					if (from != "savestate" && from != "power_on")
						return errorJson("from must be savestate or power_on");
					bool authorOk = !author.empty() && author.size() <= 254;
					for (char c : author)
						authorOk = authorOk && c >= 0x20 && c <= 0x7e;
					if (!authorOk)
						return errorJson("author must be 1 to 254 printable ASCII characters");
				}
				if (!VMManager::HasValidVM())
					return errorJson("no game is running");
				if (!runOnCpuThread(readNow))
					return errorJson("the CPU thread did not answer within 5 s");
				if (now->active)
					return errorJson("an input recording is active already (" + now->path + "): stop it first");
				if (action == "new")
				{
					if (FileSystem::FileExists(path.c_str()) && !getBool(params, "overwrite", false))
						return errorJson("the file exists already: " + path + " (overwrite to replace it)");
					FileSystem::CreateDirectoryPath(std::string(Path::GetDirectory(path)).c_str(), true);
				}
				else if (!FileSystem::FileExists(path.c_str()))
					return errorJson("no such recording: " + path);
				if (!insideDataRoot(path))
					return errorJson("the recording's folder leads outside PCSX2's data folder: " + path);
				// Loading or saving the start state can take a while
				auto started = std::make_shared<bool>(false);
				const bool savestate = from == "savestate";
				if (!runOnCpuThread([action, path, savestate, author, started, readNow]() {
						*started = action == "new" ? g_InputRecording.create(path, savestate, author) : g_InputRecording.play(path);
						if (*started)
						{
							// A replay is closed at its last frame rather than recorded on (EndInputRecordingReplay)
							s_replayStopsAtEnd = action == "play";
							s_replayPath = g_InputRecording.getData().getFilename();
							s_replayEnded = false;
						}
						readNow();
					}, std::chrono::seconds(30)))
					return errorJson("the CPU thread did not answer within 30 s");
				if (!*started)
				{
					const std::string state = path + "_SaveState.p2s";
					if (action == "new")
						return errorJson("PCSX2 could not create the recording: " + path + " (see its log)");
					return errorJson("PCSX2 could not play the recording: " + path +
									 (FileSystem::FileExists(state.c_str()) ? " (see its log: not a recording, or its savestate would not load)" :
																			  " (see its log: not a recording, or one from a savestate whose " + state + " is missing)"));
				}
			}
			else if (action == "stop")
			{
				// Only an active recording is stopped: a stop queued while none is active would close the next one
				if (!runOnCpuThread([readNow]() {
						readNow();
						if (g_InputRecording.isActive())
						{
							s_replayStopsAtEnd = false;
							s_replayEnded = false;
							g_InputRecording.stop();
						}
					}))
					return errorJson("the CPU thread did not answer within 5 s");
				if (!now->active && !now->replayEnded)
					return errorJson("no input recording is active");
				const bool endedBefore = !now->active;
				const std::string stopped = endedBefore ? now->ended.path : now->path;
				// While running PCSX2 closes the file at the end of the frame
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				while (!endedBefore)
				{
					if (!runOnCpuThread(readNow))
						return errorJson("the CPU thread did not answer within 5 s");
					if (!now->active || std::chrono::steady_clock::now() >= deadline)
						break;
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
				if (now->active)
					return errorJson("PCSX2 closes the recording at the end of a frame, and none ended within 5 s (is the debugger holding emulation?): check with action status");
				// The recording's final length and the frame it stopped at; a replay that reached its last frame was
				// closed there already, and its numbers are the ones it ended with
				j.startObject();
				j.kv("ok", true);
				j.kv("mode", "off");
				j.kv("stopped", stopped);
				j.kv("replay_ended", endedBefore);
				j.kv("recording_frame", (int64_t)(endedBefore ? now->ended.frame : now->frame));
				j.kv("total_frames", (int64_t)(endedBefore ? now->ended.totalFrames : now->totalFrames));
				j.kv("emulation", vmStateName(VMManager::GetState()));
				j.kv("frame", (int64_t)g_FrameCount);
				j.endObject();
				return j.str();
			}
			else if (VMManager::HasValidVM())
			{
				if (!runOnCpuThread(readNow))
					return errorJson("the CPU thread did not answer within 5 s");
			}
			j.startObject();
			j.kv("ok", true);
			j.kv("mode", !now->active ? "off" : now->recording ? "recording" : "replaying");
			if (now->active)
			{
				j.kv("path", now->path);
				j.kv("from", now->fromSavestate ? "savestate" : "power_on");
				if (now->fromSavestate)
				{
					const std::string state = now->path + "_SaveState.p2s";
					j.kv("savestate", state);
					// PCSX2 writes a new recording's savestate in the background
					j.kv("savestate_exists", FileSystem::FileExists(state.c_str()));
				}
				j.kv("author", now->author);
				j.kv("game", now->game);
				j.kv("recording_frame", (int64_t)now->frame);
				j.kv("total_frames", (int64_t)now->totalFrames);
				j.kv("undo_count", (int64_t)now->undoCount);
				j.kv("starting_frame", (int64_t)now->startingFrame);
			}
			else if (now->replayEnded)
			{
				// The last replay this server started reached its last frame and was closed there, unchanged
				j.kv("replay_ended", true);
				j.kv("path", now->ended.path);
				j.kv("recording_frame", (int64_t)now->ended.frame);
				j.kv("total_frames", (int64_t)now->ended.totalFrames);
				j.kv("ended_at_frame", (int64_t)now->ended.endedAt);
			}
			j.kv("emulation", vmStateName(VMManager::GetState()));
			j.kv("frame", (int64_t)g_FrameCount);
			j.endObject();
		}
		// ----- UNKNOWN COMMAND -----
		else
		{
			j.startObject();
			j.kv("ok", false);
			j.kv("error", "Unknown command: " + cmd);
			j.key("available_commands"); j.startArray();
			const char* cmds[] = {
				"status", "read_registers", "write_register", "set_pc",
				"read_memory", "write_memory", "read_string",
				"disassemble", "evaluate",
				"set_breakpoint", "remove_breakpoint", "list_breakpoints",
				"set_memcheck", "remove_memcheck", "list_memchecks",
				"pause", "resume", "step", "step_over",
				"get_threads", "get_modules",
				"is_valid_address", "clear_breakpoints", "screenshot",
				"controller", "frame_advance", "emulation", "capture", "gs_dump",
				"input_recording"
			};
			for (const char* c : cmds) j.valStr(c);
			j.endArray();
			j.endObject();
		}

		return j.str();
	}

	// ============================================================
	// TCP Server
	// ============================================================
	static std::atomic<bool> s_running{false};
	static std::thread s_serverThread;
	static socket_t s_listenSocket = SOCKET_INVALID;

	static void clientHandler(socket_t clientSock)
	{
		std::string buffer;
		char recvBuf[4096];

		while (s_running.load())
		{
			int bytes = recv(clientSock, recvBuf, sizeof(recvBuf) - 1, 0);
			if (bytes <= 0) break;

			recvBuf[bytes] = '\0';
			buffer += recvBuf;

			// Process complete lines
			size_t newlinePos;
			while ((newlinePos = buffer.find('\n')) != std::string::npos)
			{
				std::string line = buffer.substr(0, newlinePos);
				buffer = buffer.substr(newlinePos + 1);

				// Trim
				while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
					line.pop_back();

				if (line.empty()) continue;

				std::string response = handleCommand(line);
				response += "\n";

				send(clientSock, response.c_str(), (int)response.size(), 0);
			}
		}

		CLOSE_SOCKET(clientSock);
	}

	// The port the server listens on; 0 while it does not listen (stopped, or the port was taken).
	static std::atomic<int> s_port{0};

	static void serverLoop(int port)
	{
#ifdef _WIN32
		WSADATA wsaData;
		WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

		s_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s_listenSocket == SOCKET_INVALID)
		{
			fprintf(stderr, "[DebugServer] Failed to create socket\n");
			return;
		}

		// Claim the port for this process alone, so a second PCSX2 on the same port fails to bind instead of
		// sharing it (on Windows, SO_REUSEADDR lets several processes listen on one port and hands each
		// connection to any of them). Elsewhere SO_REUSEADDR only allows rebinding after a restart.
		int opt = 1;
#ifdef _WIN32
		setsockopt(s_listenSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&opt, sizeof(opt));
#else
		setsockopt(s_listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#endif

		struct sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only
		addr.sin_port = htons((u_short)port);

		if (bind(s_listenSocket, (struct sockaddr*)&addr, sizeof(addr)) != 0)
		{
			fprintf(stderr, "[DebugServer] Failed to bind on port %d (is another PCSX2 using it?)\n", port);
			CLOSE_SOCKET(s_listenSocket);
			s_listenSocket = SOCKET_INVALID;
			return;
		}

		if (listen(s_listenSocket, 2) != 0)
		{
			fprintf(stderr, "[DebugServer] Failed to listen\n");
			CLOSE_SOCKET(s_listenSocket);
			s_listenSocket = SOCKET_INVALID;
			return;
		}

		s_port.store(port);
		fprintf(stderr, "[DebugServer] Listening on 127.0.0.1:%d\n", port);

		while (s_running.load())
		{
			// Use select with timeout to allow clean shutdown
			fd_set readSet;
			FD_ZERO(&readSet);
			FD_SET(s_listenSocket, &readSet);

			struct timeval tv;
			tv.tv_sec = 1;
			tv.tv_usec = 0;

			int selectResult = select((int)s_listenSocket + 1, &readSet, nullptr, nullptr, &tv);
			if (selectResult <= 0) continue;

			socket_t clientSock = accept(s_listenSocket, nullptr, nullptr);
			if (clientSock == SOCKET_INVALID) continue;

			fprintf(stderr, "[DebugServer] Client connected\n");

			// Handle client in a new thread
			std::thread(clientHandler, clientSock).detach();
		}

		s_port.store(0);
		CLOSE_SOCKET(s_listenSocket);
		s_listenSocket = SOCKET_INVALID;

#ifdef _WIN32
		WSACleanup();
#endif
	}

	void Start(int port)
	{
		if (s_running.load()) return;
		s_running.store(true);
		s_serverThread = std::thread(serverLoop, port);
	}

	void Stop()
	{
		// The server thread closes its own socket within a second of this; waiting for it frees the port, so
		// a Start right after can bind it again.
		s_running.store(false);
		if (s_serverThread.joinable())
			s_serverThread.join();
	}

	bool IsRunning()
	{
		return s_running.load();
	}

	int GetPort()
	{
		return s_port.load();
	}

	void OnBreakpointHit()
	{
		// Future: notify connected clients of breakpoint events
	}

	bool EndInputRecordingReplay()
	{
		if (!s_replayStopsAtEnd || !g_InputRecording.isActive() || !g_InputRecording.getControls().isReplaying())
			return false;
		const InputRecordingFile& file = g_InputRecording.getData();
		if (file.getFilename() != s_replayPath)
		{
			// Not the replay this server started (stopped and another started from PCSX2's menu): leave it be
			s_replayStopsAtEnd = false;
			return false;
		}
		if (g_InputRecording.getFrameCounter() < file.getTotalFrames())
			return false;
		s_endedReplay = {s_replayPath, g_InputRecording.getFrameCounter(), file.getTotalFrames(), g_FrameCount};
		s_replayEnded = true;
		s_replayStopsAtEnd = false;
		// PCSX2 has just paused at the last frame, so this closes the file now, before the frame's input is handled;
		// should emulation still run, PCSX2 closes it at the start of the next frame, and until then it only reads.
		g_InputRecording.stop();
		return true;
	}

} // namespace DebugServer

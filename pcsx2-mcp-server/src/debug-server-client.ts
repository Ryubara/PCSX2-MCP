/**
 * PCSX2 Debug Server Client
 * Talks to the custom C++ JSON/TCP server inside PCSX2 (port 21512)
 * 
 * This REPLACES the GDB client — gives us EVERYTHING:
 *   - Full 128-bit registers (all 7 categories)
 *   - Native PCSX2 disassembly
 *   - Expression evaluation with symbol lookup
 *   - Conditional breakpoints
 *   - Memory watchpoints (read/write/onChange)
 *   - Step and step-over (delay slot aware)
 *   - Thread list, module list
 *   - String reading, address validation
 * 
 * Protocol: newline-delimited JSON over TCP
 * Request:  {"cmd":"...", ...}\n
 * Response: {"ok":true, ...}\n
 */

import * as net from 'node:net';

export interface DebugRegister {
  name: string;
  value: string;  // 32-char hex string (128-bit)
  display: string; // PCSX2's formatted display
}

export interface RegisterCategory {
  size: number;  // bits per register
  count: number;
  regs: DebugRegister[];
}


export interface DisasmInstruction {
  address: string;
  opcode: string;
  disasm: string;
}

export interface BreakpointInfo {
  address: string;
  enabled: boolean;
  temporary: boolean;
  stepping: boolean;
  has_condition: boolean;
  condition?: string;
  description?: string;
}

export interface MemcheckInfo {
  start: string;
  end: string;
  hits: number;
  last_pc: string;
  last_addr: string;
  description?: string;
}

export interface ThreadInfo {
  id: number;
  pc: string;
  status: number;
  wait_type: number;
}

export interface StepResult {
  old_pc: string;
  new_pc: string;
  disasm: string;
  opcode: string;
}

export interface EvalResult {
  ok: boolean;
  result?: number;
  hex?: string;
  error?: string;
}

type CpuTarget = 'ee' | 'iop';

export class DebugServerClient {
  private host: string;
  private port: number;
  private socket: net.Socket | null = null;
  private connected = false;
  private responseBuffer = '';
  private pendingResolve: ((data: any) => void) | null = null;
  private pendingReject: ((err: Error) => void) | null = null;

  constructor(host = '127.0.0.1', port = 21512) {
    this.host = host;
    this.port = port;
  }

  async connect(): Promise<void> {
    return new Promise((resolve, reject) => {
      this.socket = new net.Socket();
      this.socket.setEncoding('utf8');

      const timeout = setTimeout(() => {
        this.socket?.destroy();
        reject(new Error(`Connection timeout to DebugServer at ${this.host}:${this.port}`));
      }, 3000);

      this.socket.connect(this.port, this.host, () => {
        clearTimeout(timeout);
        this.connected = true;
        resolve();
      });

      this.socket.on('data', (data: string) => {
        this.responseBuffer += data;
        this.processBuffer();
      });

      this.socket.on('error', (err) => {
        clearTimeout(timeout);
        this.connected = false;
        if (this.pendingReject) {
          this.pendingReject(err);
          this.pendingResolve = null;
          this.pendingReject = null;
        } else {
          reject(err);
        }
      });

      this.socket.on('close', () => {
        this.connected = false;
      });
    });
  }

  private processBuffer(): void {
    const newlineIdx = this.responseBuffer.indexOf('\n');
    if (newlineIdx < 0) return;

    const line = this.responseBuffer.substring(0, newlineIdx);
    this.responseBuffer = this.responseBuffer.substring(newlineIdx + 1);

    if (this.pendingResolve) {
      try {
        const data = JSON.parse(line);
        this.pendingResolve(data);
      } catch (e) {
        if (this.pendingReject) this.pendingReject(new Error(`Invalid JSON: ${line}`));
      }
      this.pendingResolve = null;
      this.pendingReject = null;
    }
  }

  private async send(cmd: Record<string, any>, timeoutMs = 10000): Promise<any> {
    if (!this.connected || !this.socket) {
      throw new Error('Not connected to PCSX2 Debug Server');
    }

    return new Promise((resolve, reject) => {
      this.pendingResolve = resolve;
      this.pendingReject = reject;

      const json = JSON.stringify(cmd) + '\n';
      this.socket!.write(json);

      // Timeout
      setTimeout(() => {
        if (this.pendingReject === reject) {
          this.pendingResolve = null;
          this.pendingReject = null;
          reject(new Error(`Command timeout: ${cmd.cmd}`));
        }
      }, timeoutMs);
    });
  }

  disconnect(): void {
    this.socket?.destroy();
    this.socket = null;
    this.connected = false;
  }

  isConnected(): boolean { return this.connected; }

  // ===== Status =====

  async getStatus(cpu: CpuTarget = 'ee'): Promise<{ alive: boolean; paused: boolean; pc: string; cycles: number }> {
    const resp = await this.send({ cmd: 'status', cpu });
    if (!resp.ok) throw new Error(resp.error);
    return resp.data;
  }

  // ===== Registers =====

  /** Read all registers (all categories or specific one) */
  async readRegisters(cpu: CpuTarget = 'ee', category?: number): Promise<any> {
    const cmd: any = { cmd: 'read_registers', cpu };
    if (category !== undefined) cmd.category = category;
    const resp = await this.send(cmd);
    if (!resp.ok) throw new Error(resp.error);
    return resp.data;
  }

  /** Write a 128-bit register */
  async writeRegister(category: number, index: number, value: string, cpu: CpuTarget = 'ee'): Promise<void> {
    const resp = await this.send({ cmd: 'write_register', cpu, category, index, value });
    if (!resp.ok) throw new Error(resp.error);
  }

  /** Set the Program Counter */
  async setPC(value: string, cpu: CpuTarget = 'ee'): Promise<void> {
    const resp = await this.send({ cmd: 'set_pc', cpu, value });
    if (!resp.ok) throw new Error(resp.error);
  }

  // ===== Memory =====

  /** Read memory as hex string */
  async readMemory(address: string, length: number, cpu: CpuTarget = 'ee'): Promise<string> {
    const resp = await this.send({ cmd: 'read_memory', cpu, address, length });
    if (!resp.ok) throw new Error(resp.error);
    return resp.hex;
  }

  /** Read memory as Buffer */
  async readMemoryBuffer(address: string, length: number, cpu: CpuTarget = 'ee'): Promise<Buffer> {
    const hex = await this.readMemory(address, length, cpu);
    return Buffer.from(hex, 'hex');
  }

  /** Write memory from hex string */
  async writeMemory(address: string, data: string, cpu: CpuTarget = 'ee'): Promise<number> {
    const resp = await this.send({ cmd: 'write_memory', cpu, address, data });
    if (!resp.ok) throw new Error(resp.error);
    return resp.written;
  }

  /** Read a null-terminated string */
  async readString(address: string, maxLength = 256, cpu: CpuTarget = 'ee'): Promise<string> {
    const resp = await this.send({ cmd: 'read_string', cpu, address, max_length: maxLength });
    if (!resp.ok) throw new Error(resp.error);
    return resp.string;
  }

  /** Check if an address is valid */
  async isValidAddress(address: string, cpu: CpuTarget = 'ee'): Promise<boolean> {
    const resp = await this.send({ cmd: 'is_valid_address', cpu, address });
    if (!resp.ok) throw new Error(resp.error);
    return resp.valid;
  }

  // ===== Disassembly (NATIVE PCSX2!) =====

  /** Disassemble using PCSX2's own disassembler — perfect output */
  async disassemble(address: string, count = 20, simplify = true, cpu: CpuTarget = 'ee'): Promise<DisasmInstruction[]> {
    const resp = await this.send({ cmd: 'disassemble', cpu, address, count, simplify });
    if (!resp.ok) throw new Error(resp.error);
    return resp.instructions;
  }

  // ===== Expression Evaluation =====

  /** Evaluate a MIPS expression (e.g., "v0 + 0x100", "gp + 0x20") with symbol support */
  async evaluate(expression: string, cpu: CpuTarget = 'ee'): Promise<EvalResult> {
    const resp = await this.send({ cmd: 'evaluate', cpu, expression });
    return resp;
  }

  // ===== Breakpoints =====

  /** Set a breakpoint (optionally with condition expression and description) */
  async setBreakpoint(address: string, options?: { condition?: string; description?: string; temporary?: boolean; cpu?: CpuTarget }): Promise<void> {
    const resp = await this.send({
      cmd: 'set_breakpoint',
      cpu: options?.cpu || 'ee',
      address,
      condition: options?.condition,
      description: options?.description,
      temporary: options?.temporary ?? false,
    });
    if (!resp.ok) throw new Error(resp.error);
  }

  async removeBreakpoint(address: string, cpu: CpuTarget = 'ee'): Promise<void> {
    const resp = await this.send({ cmd: 'remove_breakpoint', cpu, address });
    if (!resp.ok) throw new Error(resp.error);
  }

  async listBreakpoints(cpu: CpuTarget = 'ee'): Promise<BreakpointInfo[]> {
    const resp = await this.send({ cmd: 'list_breakpoints', cpu });
    if (!resp.ok) throw new Error(resp.error);
    return resp.breakpoints;
  }

  // ===== Memory Watchpoints =====

  /** Set a memory watchpoint (read/write/access/onchange) with optional condition */
  async setMemcheck(address: string, end: string, options?: {
    type?: 'read' | 'write' | 'readwrite' | 'onchange';
    action?: 'break' | 'log' | 'both';
    condition?: string;
    description?: string;
    cpu?: CpuTarget;
  }): Promise<void> {
    const resp = await this.send({
      cmd: 'set_memcheck',
      cpu: options?.cpu || 'ee',
      address,
      end,
      type: options?.type || 'write',
      action: options?.action || 'break',
      condition: options?.condition,
      description: options?.description,
    });
    if (!resp.ok) throw new Error(resp.error);
  }

  async removeMemcheck(address: string, end: string, cpu: CpuTarget = 'ee'): Promise<void> {
    const resp = await this.send({ cmd: 'remove_memcheck', cpu, address, end });
    if (!resp.ok) throw new Error(resp.error);
  }

  async listMemchecks(cpu: CpuTarget = 'ee'): Promise<MemcheckInfo[]> {
    const resp = await this.send({ cmd: 'list_memchecks', cpu });
    if (!resp.ok) throw new Error(resp.error);
    return resp.memchecks;
  }

  // ===== Execution Control =====

  async pause(cpu: CpuTarget = 'ee'): Promise<string> {
    const resp = await this.send({ cmd: 'pause', cpu });
    if (!resp.ok) throw new Error(resp.error);
    return resp.pc;
  }

  async resume(cpu: CpuTarget = 'ee'): Promise<void> {
    const resp = await this.send({ cmd: 'resume', cpu });
    if (!resp.ok) throw new Error(resp.error);
  }

  /** Single-step one instruction (delay slot aware) */
  async step(cpu: CpuTarget = 'ee'): Promise<StepResult> {
    const resp = await this.send({ cmd: 'step', cpu });
    if (!resp.ok) throw new Error(resp.error);
    return resp;
  }

  /** Step over a JAL/JALR — effectively "next" */
  async stepOver(cpu: CpuTarget = 'ee'): Promise<StepResult> {
    const resp = await this.send({ cmd: 'step_over', cpu });
    if (!resp.ok) throw new Error(resp.error);
    return resp;
  }

  // ===== Thread/Module Info =====

  async getThreads(cpu: CpuTarget = 'ee'): Promise<ThreadInfo[]> {
    const resp = await this.send({ cmd: 'get_threads', cpu });
    if (!resp.ok) throw new Error(resp.error);
    return resp.threads;
  }

  async getModules(cpu: CpuTarget = 'iop'): Promise<Array<{ name: string; version: number }>> {
    const resp = await this.send({ cmd: 'get_modules', cpu });
    if (!resp.ok) throw new Error(resp.error);
    return resp.modules;
  }

  /** Get call stack backtrace using PCSX2's MipsStackWalk */
  async getBacktrace(cpu: CpuTarget = 'ee', maxFrames = 32): Promise<Array<{ entry: string; pc: string; sp: string; stack_size: number; disasm: string }>> {
    const resp = await this.send({ cmd: 'get_backtrace', cpu, max_frames: maxFrames });
    if (!resp.ok) throw new Error(resp.error);
    return resp.frames;
  }

  // ===== Bulk Operations =====

  async clearAllBreakpoints(): Promise<void> {
    const resp = await this.send({ cmd: 'clear_breakpoints' });
    if (!resp.ok) throw new Error(resp.error);
  }

  // ===== Screenshot =====

  /** Saves the picture PCSX2 shows now as a PNG in its snapshots folder; paused or running. */
  async screenshot(name?: string): Promise<{ path: string; width: number; height: number }> {
    const resp = await this.send(name ? { cmd: 'screenshot', name } : { cmd: 'screenshot' });
    if (!resp.ok) throw new Error(resp.error);
    return { path: resp.path, width: resp.width, height: resp.height };
  }

  // ===== Controller and frames =====

  /** Sets a DualShock 2's whole state: these buttons held, all others released; sticks 0-255, 127 = centre. */
  async controller(state: { port?: number; buttons?: string[]; lx?: number; ly?: number; rx?: number; ry?: number }): Promise<{ frame: number; held: string[] }> {
    const resp = await this.send({ cmd: 'controller', ...state, buttons: (state.buttons ?? []).join(',') });
    if (!resp.ok) throw new Error(resp.error);
    return { frame: resp.frame, held: resp.held };
  }

  /** Runs this many frames, then pauses; answers once paused. */
  async frameAdvance(frames: number): Promise<{ before: number; frame: number }> {
    // The server waits up to 50 ms a frame plus 5 s for the pause
    const resp = await this.send({ cmd: 'frame_advance', frames }, frames * 50 + 10000);
    if (!resp.ok) throw new Error(resp.error);
    return { before: resp.frame_before, frame: resp.frame };
  }

  /** Pauses or resumes emulation as a whole (not the debugger's CPU break), or reports its state. */
  async emulation(action: 'pause' | 'resume' | 'status'): Promise<{ state: string; frame: number }> {
    const resp = await this.send({ cmd: 'emulation', action });
    if (!resp.ok) throw new Error(resp.error);
    return { state: resp.state, frame: resp.frame };
  }

  /** Starts PCSX2's own video capture into its videos folder, stops it (waiting for the encoder), or reports it. */
  async capture(action: 'start' | 'stop' | 'status', name?: string): Promise<{ capturing: boolean; path?: string; video?: boolean; audio?: boolean; elapsed?: string; frame: number }> {
    const resp = await this.send(name ? { cmd: 'capture', action, name } : { cmd: 'capture', action }, action === 'stop' ? 35000 : 10000);
    if (!resp.ok) throw new Error(resp.error);
    return { capturing: resp.capturing, path: resp.path, video: resp.video, audio: resp.audio, elapsed: resp.elapsed, frame: resp.frame };
  }

  /** Starts PCSX2's own GS dump of this many frames into its snapshots folder, or reports the last one. */
  async gsDump(action: 'start' | 'status', frames = 1, name?: string, wait = true): Promise<{ state: 'none' | 'queued' | 'recording' | 'done' | 'dropped'; path?: string; screenshot?: string; frames?: number; emulation?: string; frame: number }> {
    const cmd: Record<string, any> = action === 'start' ? { cmd: 'gs_dump', action, frames, wait } : { cmd: 'gs_dump', action };
    if (action === 'start' && name) cmd.name = name;
    // A waiting start takes up to 50 ms a frame plus 5 s on the server
    const resp = await this.send(cmd, action === 'start' && wait ? frames * 50 + 15000 : 10000);
    if (!resp.ok) throw new Error(resp.error);
    return { state: resp.state, path: resp.path, screenshot: resp.screenshot, frames: resp.frames, emulation: resp.emulation, frame: resp.frame };
  }

  /** Drives PCSX2's own input recording (.p2m2): new, play, stop, or status. */
  async inputRecording(action: 'new' | 'play' | 'stop' | 'status', opts: { path?: string; author?: string; from?: 'savestate' | 'power_on'; overwrite?: boolean } = {}): Promise<InputRecordingStatus> {
    const cmd: Record<string, any> = { cmd: 'input_recording', action };
    if (action === 'new' || action === 'play') {
      if (opts.path) cmd.path = opts.path;
    }
    if (action === 'new') {
      cmd.author = opts.author;
      cmd.from = opts.from ?? 'savestate';
      cmd.overwrite = opts.overwrite ?? false;
    }
    // new and play save or load the start state on the server (up to 30 s); stop waits up to 5 s for a frame to end
    const resp = await this.send(cmd, action === 'new' || action === 'play' ? 40000 : action === 'stop' ? 15000 : 10000);
    if (!resp.ok) throw new Error(resp.error);
    return {
      mode: resp.mode, path: resp.path, from: resp.from, savestate: resp.savestate, savestateExists: resp.savestate_exists,
      author: resp.author, game: resp.game, recordingFrame: resp.recording_frame, totalFrames: resp.total_frames,
      undoCount: resp.undo_count, startingFrame: resp.starting_frame, stopped: resp.stopped, replayEnded: resp.replay_ended, endedAtFrame: resp.ended_at_frame,
      emulation: resp.emulation, frame: resp.frame,
    };
  }
}

/** What PCSX2's input recording holds, as the debug server's input_recording command answers. */
export interface InputRecordingStatus {
  mode: 'off' | 'recording' | 'replaying';
  path?: string;
  from?: 'savestate' | 'power_on';
  savestate?: string;
  savestateExists?: boolean;
  author?: string;
  game?: string;
  recordingFrame?: number;
  totalFrames?: number;
  undoCount?: number;
  startingFrame?: number;
  stopped?: string;
  /** The last replay started here reached its last frame and was closed there, unchanged. */
  replayEnded?: boolean;
  /** PCSX2's frame counter when that replay ended (status only). */
  endedAtFrame?: number;
  emulation: string;
  frame: number;
}

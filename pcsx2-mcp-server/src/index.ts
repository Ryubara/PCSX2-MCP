#!/usr/bin/env node
/**
 * PCSX2 ULTIMATE MCP Server v2.0
 * 
 * 3-tier connection priority:
 *   1. Custom DebugServer (port 21512) — FULL 128-bit, native disasm, expressions, conditional BP
 *   2. Pine IPC (port 28011) — memory R/W, game info, save states (vanilla PCSX2)
 *   3. Standalone — PS2Recomp project tools only
 * 
 * 30+ tools across categories:
 *   Connection, Memory, Registers, Disassembly, Expression Eval,
 *   Breakpoints, Watchpoints, Stepping, Threads, Game Info,
 *   Save States, Pattern Search, Memory Diff, PS2Recomp Integration
 */

import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { z } from 'zod';
import { DebugServerClient } from './debug-server-client.js';
import { PineClient, EmuStatus } from './pine-client.js';
import * as fs from 'node:fs';
import * as path from 'node:path';

// ===== State =====
let debugServer: DebugServerClient | null = null;
let pine: PineClient | null = null;
const memSnapshots = new Map<string, { addr: number; data: Buffer }>();
const PS2RECOMP_ROOT = process.env.PS2RECOMP_ROOT || 'E:\\Programmi VARI\\PROGETTI\\PS2Recomp';

// ===== Helpers =====
function parseAddr(s: string): number { return parseInt(s.replace(/^0x/i, ''), 16); }

function hexDump(buf: Buffer, base: number): string {
  const lines: string[] = [];
  for (let i = 0; i < buf.length; i += 16) {
    const addr = (base + i).toString(16).padStart(8, '0');
    const hex: string[] = [];
    let ascii = '';
    for (let j = 0; j < 16; j++) {
      if (i + j < buf.length) {
        hex.push(buf[i + j].toString(16).padStart(2, '0'));
        const c = buf[i + j];
        ascii += (c >= 0x20 && c < 0x7F) ? String.fromCharCode(c) : '.';
      } else { hex.push('  '); ascii += ' '; }
    }
    lines.push(`${addr}  ${hex.slice(0, 8).join(' ')}  ${hex.slice(8).join(' ')}  |${ascii}|`);
  }
  return lines.join('\n');
}

function hasDebug(): boolean { return debugServer?.isConnected() ?? false; }  
function hasPine(): boolean { return pine?.isConnected() ?? false; }

async function readMem(addr: number, len: number): Promise<Buffer> {
  if (hasDebug()) return debugServer!.readMemoryBuffer('0x' + addr.toString(16), len);
  if (hasPine()) return pine!.readMemory(addr, len);
  throw new Error('No connection — use pcsx2_connect first');
}

async function writeMem(addr: number, data: Buffer): Promise<void> {
  if (hasDebug()) { await debugServer!.writeMemory('0x' + addr.toString(16), data.toString('hex')); return; }
  if (hasPine()) { await pine!.writeMemory(addr, data); return; }
  throw new Error('No connection');
}

// ===== MCP Server =====
const server = new McpServer({ name: 'pcsx2-ultimate-debugger', version: '2.0.0' }, { capabilities: { tools: {}, resources: {} } });

// ==========================================================
//  TOOL: pcsx2_connect
// ==========================================================
server.tool('pcsx2_connect',
  'Connect to PCSX2. Tries DebugServer (21512), then Pine (28011). DebugServer gives FULL access (128-bit regs, expressions, conditional BP, native disasm). Pine gives memory + game info.',
  { debug_port: z.number().default(21512).describe('DebugServer port'), pine_port: z.number().default(28011).describe('Pine IPC port'), mode: z.enum(['auto', 'debug', 'pine']).default('auto') },
  async ({ debug_port, pine_port, mode }) => {
    const results: string[] = [];
    // Try DebugServer
    if (mode === 'auto' || mode === 'debug') {
      try {
        debugServer = new DebugServerClient('127.0.0.1', debug_port);
        await debugServer.connect();
        const st = await debugServer.getStatus();
        results.push(`✅ DebugServer: connected (PC=0x${st.pc}, paused=${st.paused})`);
        results.push('   → 128-bit registers, native disasm, expressions, conditional BP, step-over, threads');
      } catch (e: any) {
        debugServer = null;
        results.push(`❌ DebugServer (port ${debug_port}): ${e.message}`);
      }
    }
    // Try Pine
    if (mode === 'auto' || mode === 'pine') {
      try {
        pine = new PineClient('127.0.0.1', pine_port);
        await pine.connect();
        const title = await pine.getTitle();
        results.push(`✅ Pine IPC: connected (${title})`);
      } catch (e: any) {
        pine = null;
        results.push(`❌ Pine (port ${pine_port}): ${e.message}`);
      }
    }
    if (!hasDebug() && !hasPine()) {
      results.push('\n⚠️  No connections. Make sure PCSX2 is running.');
      results.push('For DebugServer: patch PCSX2 with pcsx2-plugin/DebugServer.cpp');
      results.push('For Pine: enable IPC in PCSX2 settings');
    }
    return { content: [{ type: 'text' as const, text: results.join('\n') }] };
  }
);

// ==========================================================
//  TOOL: pcsx2_status
// ==========================================================
server.tool('pcsx2_status', 'Get connection + emulator status.', {},
  async () => {
    const p: string[] = [];
    p.push(`DebugServer: ${hasDebug() ? '✅ connected' : '❌ not connected'}`);
    p.push(`Pine IPC:    ${hasPine() ? '✅ connected' : '❌ not connected'}`);
    if (hasDebug()) {
      try { const s = await debugServer!.getStatus(); p.push(`EE PC: ${s.pc} | Paused: ${s.paused} | Cycles: ${s.cycles}`); } catch {}
    }
    if (hasPine()) {
      try { const t = await pine!.getTitle(); const id = await pine!.getID(); p.push(`Game: ${t} (${id})`); } catch {}
    }
    return { content: [{ type: 'text' as const, text: p.join('\n') }] };
  }
);

// ==========================================================
//  TOOL: pcsx2_read_memory
// ==========================================================
server.tool('pcsx2_read_memory', 'Read PS2 memory. Returns hex dump.',
  { address: z.string(), length: z.number().min(1).max(4096).default(256), format: z.enum(['hexdump', 'hex', 'u32_array', 'ascii']).default('hexdump') },
  async ({ address, length, format }) => {
    try {
      const addr = parseAddr(address);
      const data = await readMem(addr, length);
      let text: string;
      if (format === 'hexdump') text = hexDump(data, addr);
      else if (format === 'hex') text = data.toString('hex');
      else if (format === 'u32_array') {
        const v: string[] = [];
        for (let i = 0; i + 3 < data.length; i += 4) v.push('0x' + data.readUInt32LE(i).toString(16).padStart(8, '0'));
        text = v.join(', ');
      } else text = data.toString('ascii').replace(/[^\x20-\x7E]/g, '.');
      return { content: [{ type: 'text' as const, text: `Memory at 0x${addr.toString(16)} (${length}B):\n\n${text}` }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOL: pcsx2_write_memory
// ==========================================================
server.tool('pcsx2_write_memory', 'Write hex data to PS2 memory. USE WITH CAUTION.',
  { address: z.string(), data: z.string().describe('Hex data e.g. "0102030405"') },
  async ({ address, data }) => {
    try {
      const addr = parseAddr(address);
      const buf = Buffer.from(data.replace(/\s/g, ''), 'hex');
      await writeMem(addr, buf);
      return { content: [{ type: 'text' as const, text: `Wrote ${buf.length} bytes to 0x${addr.toString(16)}` }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOL: pcsx2_read_string  
// ==========================================================
server.tool('pcsx2_read_string', 'Read null-terminated string from PS2 memory.',
  { address: z.string(), max_length: z.number().default(256) },
  async ({ address, max_length }) => {
    try {
      if (hasDebug()) {
        const str = await debugServer!.readString(address, max_length);
        return { content: [{ type: 'text' as const, text: `"${str}" (${str.length} chars)` }] };
      }
      const addr = parseAddr(address);
      const data = await readMem(addr, max_length);
      const idx = data.indexOf(0);
      const str = data.subarray(0, idx >= 0 ? idx : data.length).toString('ascii');
      return { content: [{ type: 'text' as const, text: `"${str}" (${str.length} chars)` }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOL: pcsx2_read_registers (DebugServer - FULL 128-bit!)
// ==========================================================
server.tool('pcsx2_read_registers',
  'Read ALL EE registers — FULL 128-bit values. Categories: GPR, CP0, FPR, FCR, VU0F, VU0I, GSPRIV. Requires DebugServer.',
  { category: z.number().min(-1).max(6).default(-1).describe('-1 for all, 0=GPR, 1=CP0, 2=FPR, 3=FCR, 4=VU0F, 5=VU0I, 6=GSPRIV'), cpu: z.enum(['ee', 'iop']).default('ee') },
  async ({ category, cpu }) => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected. Patch PCSX2 with pcsx2-plugin/DebugServer.cpp' }], isError: true };
    try {
      const cat = category >= 0 ? category : undefined;
      const data = await debugServer!.readRegisters(cpu, cat);
      // Format nicely
      const lines: string[] = [`=== ${cpu.toUpperCase()} Registers ===`, ''];
      for (const [catName, catData] of Object.entries(data)) {
        if (catName === 'pc' || catName === 'hi' || catName === 'lo') continue;
        const cd = catData as any;
        if (!cd.regs) continue;
        lines.push(`--- ${catName} (${cd.size}-bit × ${cd.count}) ---`);
        for (const reg of cd.regs) {
          lines.push(`  ${(reg.name as string).padEnd(10)} = ${reg.display}`);
        }
        lines.push('');
      }
      if (data.pc) lines.push(`PC = ${data.pc}`);
      if (data.hi) lines.push(`HI = ${data.hi}`);
      if (data.lo) lines.push(`LO = ${data.lo}`);
      return { content: [{ type: 'text' as const, text: lines.join('\n') }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOL: pcsx2_write_register
// ==========================================================
server.tool('pcsx2_write_register', 'Write a register value (supports full 128-bit hex). Requires DebugServer.',
  { category: z.number().default(0).describe('0=GPR, 1=CP0, 2=FPR, etc.'), index: z.number().describe('Register index within category'), value: z.string().describe('Hex value (up to 128-bit)'), cpu: z.enum(['ee', 'iop']).default('ee') },
  async ({ category, index, value, cpu }) => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try {
      await debugServer!.writeRegister(category, index, value, cpu);
      return { content: [{ type: 'text' as const, text: `Set cat=${category} reg=${index} = ${value}` }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOL: pcsx2_disassemble (NATIVE PCSX2!)  
// ==========================================================
server.tool('pcsx2_disassemble', 'Disassemble MIPS instructions using PCSX2\'s NATIVE disassembler — perfect output. Requires DebugServer.',
  { address: z.string(), count: z.number().min(1).max(200).default(20), cpu: z.enum(['ee', 'iop']).default('ee') },
  async ({ address, count, cpu }) => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try {
      const instrs = await debugServer!.disassemble(address, count, true, cpu);
      const text = instrs.map(i => `${i.address}:  ${(i.opcode as string).padEnd(12)}  ${i.disasm}`).join('\n');
      return { content: [{ type: 'text' as const, text: `Disassembly (${count} instructions):\n\n${text}` }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOL: pcsx2_evaluate (EXPRESSION EVAL!)
// ==========================================================
server.tool('pcsx2_evaluate', 'Evaluate a MIPS expression with full symbol support. Examples: "v0 + 0x100", "gp + 0x20", "sp - 4". Requires DebugServer.',
  { expression: z.string().describe('Expression to evaluate'), cpu: z.enum(['ee', 'iop']).default('ee') },
  async ({ expression, cpu }) => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try {
      const r = await debugServer!.evaluate(expression, cpu);
      if (r.ok) return { content: [{ type: 'text' as const, text: `"${expression}" = ${r.hex} (${r.result})` }] };
      else return { content: [{ type: 'text' as const, text: `Eval error: ${r.error}` }], isError: true };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOL: pcsx2_set_breakpoint (with CONDITIONAL!)
// ==========================================================
server.tool('pcsx2_set_breakpoint', 'Set a breakpoint at an address. Supports conditional expressions! Requires DebugServer.',
  { address: z.string(), condition: z.string().optional().describe('Break only when expression is true, e.g. "v0 == 0x42"'), description: z.string().optional(), temporary: z.boolean().default(false) },
  async ({ address, condition, description, temporary }) => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try {
      await debugServer!.setBreakpoint(address, { condition, description, temporary });
      let msg = `Breakpoint set at ${address}`;
      if (condition) msg += ` [condition: ${condition}]`;
      return { content: [{ type: 'text' as const, text: msg }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

server.tool('pcsx2_remove_breakpoint', 'Remove a breakpoint.',
  { address: z.string() },
  async ({ address }) => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try { await debugServer!.removeBreakpoint(address); return { content: [{ type: 'text' as const, text: `Breakpoint removed at ${address}` }] }; }
    catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

server.tool('pcsx2_list_breakpoints', 'List all breakpoints with their conditions and hit status.',
  {},
  async () => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try {
      const bps = await debugServer!.listBreakpoints();
      if (bps.length === 0) return { content: [{ type: 'text' as const, text: 'No breakpoints set.' }] };
      const lines = bps.map(bp => {
        let s = `${bp.address} ${bp.enabled ? '✅' : '❌'}`;
        if (bp.has_condition) s += ` [cond: ${bp.condition}]`;
        if (bp.description) s += ` — ${bp.description}`;
        if (bp.temporary) s += ' (temp)';
        return s;
      });
      return { content: [{ type: 'text' as const, text: `${bps.length} breakpoint(s):\n${lines.join('\n')}` }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOL: pcsx2_set_watchpoint (with onChange!)
// ==========================================================
server.tool('pcsx2_set_watchpoint', 'Set a memory watchpoint. Supports read/write/access/onchange + optional condition expression!',
  { address: z.string(), end: z.string().optional().describe('End address (default: address+4)'), type: z.enum(['read', 'write', 'readwrite', 'onchange']).default('write'), action: z.enum(['break', 'log', 'both']).default('break'), condition: z.string().optional(), description: z.string().optional() },
  async ({ address, end, type, action, condition, description }) => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try {
      const endAddr = end || '0x' + (parseAddr(address) + 4).toString(16);
      await debugServer!.setMemcheck(address, endAddr, { type, action, condition, description });
      return { content: [{ type: 'text' as const, text: `Watchpoint (${type}/${action}) set at ${address}-${endAddr}${condition ? ` [cond: ${condition}]` : ''}` }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

server.tool('pcsx2_remove_watchpoint', 'Remove a memory watchpoint.',
  { address: z.string(), end: z.string().optional() },
  async ({ address, end }) => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try {
      const endAddr = end || '0x' + (parseAddr(address) + 4).toString(16);
      await debugServer!.removeMemcheck(address, endAddr);
      return { content: [{ type: 'text' as const, text: `Watchpoint removed at ${address}-${endAddr}` }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

server.tool('pcsx2_list_watchpoints', 'List all memory watchpoints with hit counts.',
  {},
  async () => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try {
      const mcs = await debugServer!.listMemchecks();
      if (mcs.length === 0) return { content: [{ type: 'text' as const, text: 'No watchpoints set.' }] };
      const lines = mcs.map(mc => `${mc.start}-${mc.end} | ${mc.hits} hits | last_PC=${mc.last_pc}${mc.description ? ` — ${mc.description}` : ''}`);
      return { content: [{ type: 'text' as const, text: `${mcs.length} watchpoint(s):\n${lines.join('\n')}` }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOL: pcsx2_step / step_over / continue / pause
// ==========================================================
server.tool('pcsx2_step', 'Execute one MIPS instruction. Returns new PC + native disasm. Requires DebugServer.',
  { count: z.number().min(1).max(100).default(1), show_registers: z.boolean().default(false) },
  async ({ count, show_registers }) => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try {
      const results: string[] = [];
      for (let i = 0; i < count; i++) {
        const r = await debugServer!.step();
        results.push(`Step ${i + 1}: PC=${r.new_pc}  ${r.opcode}  ${r.disasm}`);
      }
      if (show_registers) {
        const regs = await debugServer!.readRegisters('ee', 0); // GPR only
        results.push('', '--- GPR ---');
        for (const reg of (regs as any).GPR?.regs || [])
          results.push(`  ${(reg.name as string).padEnd(6)} = ${reg.display}`);
      }
      return { content: [{ type: 'text' as const, text: results.join('\n') }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

server.tool('pcsx2_step_over', 'Step OVER a JAL/JALR call — like "next" in a debugger. Requires DebugServer.',
  {},
  async () => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try {
      const r = await debugServer!.stepOver();
      return { content: [{ type: 'text' as const, text: `Stepped over: ${r.old_pc} → ${r.new_pc}\n${r.disasm}` }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

server.tool('pcsx2_continue', 'Resume execution until breakpoint or halt.',
  {},
  async () => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try { await debugServer!.resume(); return { content: [{ type: 'text' as const, text: 'Resumed. Use pcsx2_pause to stop.' }] }; }
    catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

server.tool('pcsx2_pause', 'Pause/halt the emulator. Returns current PC.',
  {},
  async () => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try { const pc = await debugServer!.pause(); return { content: [{ type: 'text' as const, text: `Paused at PC=${pc}` }] }; }
    catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOL: pcsx2_get_threads / pcsx2_get_modules
// ==========================================================
server.tool('pcsx2_get_threads', 'List EE/IOP BIOS threads with their status.',
  { cpu: z.enum(['ee', 'iop']).default('ee') },
  async ({ cpu }) => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try {
      const threads = await debugServer!.getThreads(cpu);
      if (threads.length === 0) return { content: [{ type: 'text' as const, text: 'No threads.' }] };
      const lines = threads.map(t => `TID ${t.id}: PC=${t.pc} status=${t.status} waitType=${t.wait_type}`);
      return { content: [{ type: 'text' as const, text: `${threads.length} threads:\n${lines.join('\n')}` }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

server.tool('pcsx2_get_modules', 'List loaded IOP modules.',
  {},
  async () => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try {
      const mods = await debugServer!.getModules('iop');
      const lines = mods.map(m => `${m.name} (v${m.version})`);
      return { content: [{ type: 'text' as const, text: `${mods.length} modules:\n${lines.join('\n')}` }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOL: pcsx2_get_backtrace
// ==========================================================
server.tool('pcsx2_get_backtrace', 'Get call stack backtrace (stack walk). Shows function entry points, PCs, stack pointers, and disassembly for each frame. Requires DebugServer + paused state.',
  { cpu: z.enum(['ee', 'iop']).default('ee'), max_frames: z.number().min(1).max(128).default(32) },
  async ({ cpu, max_frames }) => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try {
      const frames = await debugServer!.getBacktrace(cpu, max_frames);
      if (frames.length === 0) return { content: [{ type: 'text' as const, text: 'No stack frames (may not be paused, or no thread running).' }] };
      const lines = frames.map((f, i) => `#${i} entry=${f.entry} pc=${f.pc} sp=${f.sp} size=${f.stack_size}  ${f.disasm}`);
      return { content: [{ type: 'text' as const, text: `Call stack (${frames.length} frames):\n${lines.join('\n')}` }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOL: pcsx2_game_info / save_state / load_state (Pine)
// ==========================================================
server.tool('pcsx2_game_info', 'Get game title, ID, version from PCSX2. Requires Pine.',
  {},
  async () => {
    if (!hasPine()) return { content: [{ type: 'text' as const, text: 'Error: Pine not connected.' }], isError: true };
    try {
      const [t, id, uuid, gv, ev, st] = await Promise.all([pine!.getTitle(), pine!.getID(), pine!.getUUID(), pine!.getGameVersion(), pine!.getVersion(), pine!.getStatus()]);
      return { content: [{ type: 'text' as const, text: `Title: ${t}\nID: ${id}\nUUID: ${uuid}\nGame: ${gv}\nPCSX2: ${ev}\nStatus: ${st === EmuStatus.Running ? 'Running' : st === EmuStatus.Paused ? 'Paused' : 'Shutdown'}` }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

server.tool('pcsx2_save_state', 'Save emulator state. Requires Pine.', { slot: z.number().min(0).max(9) },
  async ({ slot }) => {
    if (!hasPine()) return { content: [{ type: 'text' as const, text: 'Pine not connected.' }], isError: true };
    try { await pine!.saveState(slot); return { content: [{ type: 'text' as const, text: `Saved to slot ${slot}` }] }; }
    catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

server.tool('pcsx2_load_state', 'Load emulator state. Requires Pine.', { slot: z.number().min(0).max(9) },
  async ({ slot }) => {
    if (!hasPine()) return { content: [{ type: 'text' as const, text: 'Pine not connected.' }], isError: true };
    try { await pine!.loadState(slot); return { content: [{ type: 'text' as const, text: `Loaded from slot ${slot}` }] }; }
    catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOL: pcsx2_find_pattern
// ==========================================================
server.tool('pcsx2_find_pattern', 'Search PS2 memory for a hex pattern. Use ?? for wildcards.',
  { pattern: z.string(), start: z.string().default('0x00100000'), end: z.string().default('0x02000000'), max_results: z.number().default(20) },
  async ({ pattern, start, end, max_results }) => {
    try {
      const startAddr = parseAddr(start);
      const endAddr = parseAddr(end);
      const parts = pattern.replace(/\s/g, '').match(/.{2}/g) || [];
      const pat = parts.map(p => p === '??' ? null : parseInt(p, 16));
      if (pat.length === 0) return { content: [{ type: 'text' as const, text: 'Empty pattern' }], isError: true };
      const results: number[] = [];
      const chunk = 4096;
      for (let a = startAddr; a < endAddr && results.length < max_results; a += chunk) {
        let data: Buffer;
        try { data = await readMem(a, Math.min(chunk + pat.length, endAddr - a)); } catch { continue; }
        for (let i = 0; i <= data.length - pat.length && results.length < max_results; i++) {
          let ok = true;
          for (let j = 0; j < pat.length; j++) { if (pat[j] !== null && data[i + j] !== pat[j]) { ok = false; break; } }
          if (ok) results.push(a + i);
        }
      }
      if (results.length === 0) return { content: [{ type: 'text' as const, text: `No matches for "${pattern}"` }] };
      return { content: [{ type: 'text' as const, text: `${results.length} match(es):\n${results.map(a => '0x' + a.toString(16).padStart(8, '0')).join('\n')}` }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOL: pcsx2_memory_diff
// ==========================================================
server.tool('pcsx2_memory_diff', 'Snapshot-and-compare memory. First call = snapshot, second = diff.',
  { address: z.string(), length: z.number().default(256), name: z.string().default('default') },
  async ({ address, length, name }) => {
    try {
      const addr = parseAddr(address);
      const data = await readMem(addr, length);
      const key = `${name}_${addr}_${length}`;
      if (!memSnapshots.has(key)) {
        memSnapshots.set(key, { addr, data });
        return { content: [{ type: 'text' as const, text: `Snapshot "${name}" saved. Call again to diff.` }] };
      }
      const prev = memSnapshots.get(key)!;
      memSnapshots.delete(key);
      const changes: string[] = [];
      for (let i = 0; i < Math.min(prev.data.length, data.length); i++) {
        if (prev.data[i] !== data[i]) changes.push(`  +0x${i.toString(16).padStart(4, '0')} (0x${(addr + i).toString(16)}): ${prev.data[i].toString(16).padStart(2, '0')} → ${data[i].toString(16).padStart(2, '0')}`);
      }
      if (changes.length === 0) return { content: [{ type: 'text' as const, text: 'No changes.' }] };
      return { content: [{ type: 'text' as const, text: `${changes.length} byte(s) changed:\n${changes.join('\n')}` }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOL: pcsx2_clear_all_breakpoints
// ==========================================================
server.tool('pcsx2_clear_all_breakpoints', 'Clear ALL breakpoints and watchpoints.',
  {},
  async () => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try { await debugServer!.clearAllBreakpoints(); return { content: [{ type: 'text' as const, text: 'All breakpoints and watchpoints cleared.' }] }; }
    catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOL: pcsx2_screenshot
// ==========================================================
server.tool('pcsx2_screenshot', 'Save the picture PCSX2 shows now (paused or running) as a PNG in its snapshots folder. Returns the file path; open the file to look at it.',
  { name: z.string().regex(/^[A-Za-z0-9._-]{1,100}$/).optional().describe('File name (".png" is added); default mcp-<date>-<time>-<ms>') },
  async ({ name }) => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try {
      const shot = await debugServer!.screenshot(name);
      return { content: [{ type: 'text' as const, text: `Saved ${shot.width}x${shot.height} screenshot: ${shot.path}` }] };
    }
    catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOLS: pcsx2_controller / pcsx2_frame_advance / pcsx2_emulation
// ==========================================================
const BUTTONS = ['up', 'right', 'down', 'left', 'triangle', 'circle', 'cross', 'square', 'l1', 'l2', 'r1', 'r2', 'select', 'start', 'l3', 'r3'] as const;
const stick = z.number().int().min(0).max(255).default(127);

server.tool('pcsx2_controller', 'Set the whole DualShock 2 state: the buttons listed are held, every other button is released, sticks 0-255 (127 = centre). It stays until the next call. For exact timing, pause first (pcsx2_emulation) and use pcsx2_frame_advance. For getting somewhere and exploring; a measured result uses an input recording.',
  { buttons: z.array(z.enum(BUTTONS)).default([]), lx: stick, ly: stick, rx: stick, ry: stick, port: z.number().int().min(0).max(1).default(0) },
  async ({ buttons, lx, ly, rx, ry, port }) => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try {
      const r = await debugServer!.controller({ port, buttons, lx, ly, rx, ry });
      const held = r.held.length ? r.held.join(', ') : 'no buttons';
      return { content: [{ type: 'text' as const, text: `Port ${port}: ${held} held, sticks L(${lx},${ly}) R(${rx},${ry}), at frame ${r.frame}` }] };
    }
    catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

server.tool('pcsx2_frame_advance', 'Run this many frames (1-600), then pause; returns the frame counter before and after. Emulation held by the debugger (pcsx2_pause, a breakpoint) does not advance: pcsx2_continue first.',
  { frames: z.number().int().min(1).max(600).default(1) },
  async ({ frames }) => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try {
      const r = await debugServer!.frameAdvance(frames);
      return { content: [{ type: 'text' as const, text: `Advanced ${frames} frame(s): frame ${r.before} -> ${r.frame}, paused` }] };
    }
    catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

server.tool('pcsx2_emulation', 'Pause or resume emulation as a whole, or report its state and frame counter. This is the emulator pause pcsx2_frame_advance works from; pcsx2_pause is the debugger\'s CPU break.',
  { action: z.enum(['pause', 'resume', 'status']).default('status') },
  async ({ action }) => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try {
      const r = await debugServer!.emulation(action);
      return { content: [{ type: 'text' as const, text: `Emulation ${r.state} at frame ${r.frame}` }] };
    }
    catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOL: pcsx2_capture
// ==========================================================
server.tool('pcsx2_capture', 'Start PCSX2\'s own video capture into its videos folder, stop it, or report it. Codec, size and whether sound is recorded come from PCSX2\'s settings. Only presented frames are recorded: none while paused, one per frame advanced. For exact frames, pcsx2_frame_advance with pcsx2_screenshot is lossless.',
  {
    action: z.enum(['start', 'stop', 'status']).default('status'),
    name: z.string().regex(/^[A-Za-z0-9._-]{1,100}$/).optional().describe('start only: file name (the container\'s extension, mp4 by default, is added); default mcp-<date>-<time>-<ms>'),
  },
  async ({ action, name }) => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try {
      const r = await debugServer!.capture(action, action === 'start' ? name : undefined);
      const file = r.path ? `: ${r.path}` : '';
      const text = r.capturing
        ? `Capturing ${r.video ? 'video' : ''}${r.video && r.audio ? ' and ' : ''}${r.audio ? 'audio' : ''} for ${r.elapsed}${file}, at frame ${r.frame}`
        : `${action === 'stop' ? 'Capture stopped' : 'Not capturing'}${file}, at frame ${r.frame}`;
      return { content: [{ type: 'text' as const, text }] };
    }
    catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOL: pcsx2_gs_dump
// ==========================================================
server.tool('pcsx2_gs_dump', 'Save PCSX2\'s own GS dump (the .gs file of its "Save Single/Multi Frame GS Dump" hotkeys, compressed as its settings choose, with its screenshot beside it) of N frames into its snapshots folder, or report the last one. PCSX2 starts the dump at the next frame and ends it after N, so while emulation is paused it stays queued: resume or pcsx2_frame_advance (about N + 5 frames: PCSX2 adds a few to close the dump), then call with action status. Returns the file\'s path and its state.',
  {
    action: z.enum(['start', 'status']).default('start'),
    frames: z.number().int().min(1).max(300).default(1).describe('start only: frames to dump, 1-300'),
    name: z.string().regex(/^[A-Za-z0-9._-]{1,100}$/).optional().describe('start only: file name without extension (.gs, .gs.xz or .gs.zst is added); default PCSX2\'s own <title>_<serial>_<time>'),
    wait: z.boolean().default(true).describe('start only: while emulation runs, answer once the dump is written'),
  },
  async ({ action, frames, name, wait }) => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    try {
      const r = await debugServer!.gsDump(action, frames, name, wait);
      const said: Record<string, string> = {
        none: 'No GS dump started yet',
        queued: 'GS dump queued: it starts at the next frame (resume or frame advance, then check with action status)',
        recording: 'GS dump being written: let frames run, then check with action status',
        done: 'GS dump saved',
        dropped: 'GS dump not taken: PCSX2 had queued a screenshot or dump of its own, or the game closed',
      };
      const files = r.path ? `: ${r.path} (${r.frames} frame${r.frames === 1 ? '' : 's'}; screenshot ${r.screenshot})` : '';
      const where = r.emulation ? `, emulation ${r.emulation}` : '';
      return { content: [{ type: 'text' as const, text: `${said[r.state] ?? r.state}${files}${where}, at frame ${r.frame}` }], isError: r.state === 'dropped' };
    }
    catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  TOOL: pcsx2_input_recording
// ==========================================================
server.tool('pcsx2_input_recording', 'PCSX2\'s own input recording (.p2m2, as its "Input Recording" menu makes and plays): new records every frame\'s controller input (pcsx2_controller\'s too) from the current point, saving PCSX2\'s start savestate beside it as <file>.p2m2_SaveState.p2s (from savestate), or from a reset of the game (from power_on); play loads that savestate (or resets the game) and replays the inputs; at the last frame PCSX2 pauses and the replay stops on its own (file closed unchanged, mode off, emulation left paused), so playing never adds to a recording, and status then reports that the replay ended; stop closes the file and answers its total frames and the frame it stopped at; status reports the mode, file, frame, total frames and undo count. Paths are relative to PCSX2\'s data folder (a portable install\'s own folder).',
  {
    action: z.enum(['new', 'play', 'stop', 'status']).default('status'),
    path: z.string().max(400).regex(/^[A-Za-z0-9._\/-]+$/).optional().describe('new and play: the recording, relative to PCSX2\'s data folder, names of letters, digits, ".", "_" and "-" split by "/" (no "." or ".."); .p2m2 is added when missing. play needs it; new defaults to recordings/mcp-<date>-<time>-<ms>.p2m2'),
    author: z.string().min(1).max(254).regex(/^[ -~]+$/).optional().describe('new only (required): the author written into the file\'s header'),
    from: z.enum(['savestate', 'power_on']).default('savestate').describe('new only: savestate starts at the current point (PCSX2 saves its start savestate beside the file), power_on resets the game and starts at boot'),
    overwrite: z.boolean().default(false).describe('new only: replace a recording that exists (PCSX2 keeps the old savestate as .bak)'),
  },
  async ({ action, path, author, from, overwrite }) => {
    if (!hasDebug()) return { content: [{ type: 'text' as const, text: 'Error: DebugServer not connected.' }], isError: true };
    if (action === 'new' && !author) return { content: [{ type: 'text' as const, text: 'Error: new needs an author.' }], isError: true };
    if (action === 'play' && !path) return { content: [{ type: 'text' as const, text: 'Error: play needs the recording\'s path.' }], isError: true };
    try {
      const r = await debugServer!.inputRecording(action, { path, author, from, overwrite });
      const at = `emulation ${r.emulation}, at frame ${r.frame}`;
      let text: string;
      const frames = `recording frame ${r.recordingFrame} of ${r.totalFrames}`;
      if (r.mode === 'off' && r.stopped) text = r.replayEnded ? `Replay had ended at its last frame and was stopped there already: ${r.stopped} (${frames}, file unchanged), ${at}` : `Input recording stopped: ${r.stopped} (${frames}), ${at}`;
      else if (r.mode === 'off' && r.replayEnded) text = `No input recording active: the replay of ${r.path} ended at its last frame and was stopped (${frames}, file unchanged; PCSX2 frame ${r.endedAtFrame} then), ${at}`;
      else if (r.mode === 'off') text = `No input recording active, ${at}`;
      else {
        const start = r.from === 'savestate' ? `from savestate ${r.savestate}${r.savestateExists ? '' : ' (still being written)'}` : 'from power-on';
        const verb = action === 'new' ? 'Recording started' : action === 'play' ? 'Replay started' : r.mode === 'recording' ? 'Recording' : 'Replaying';
        text = `${verb}: ${r.path} (${r.mode}, ${start}; author ${r.author}, game ${r.game}), ${frames}, undo count ${r.undoCount}, ${at}`;
      }
      return { content: [{ type: 'text' as const, text }] };
    }
    catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  PS2Recomp Integration Tools
// ==========================================================
server.tool('ps2recomp_lookup_function', 'Search PS2Recomp project for functions by address or name.',
  { query: z.string() },
  async ({ query }) => {
    try {
      const results: string[] = [];
      const overDir = path.join(PS2RECOMP_ROOT, 'ps2xRuntime', 'src', 'lib', 'overrides');
      if (fs.existsSync(overDir)) {
        for (const f of fs.readdirSync(overDir).filter(f => f.endsWith('.cpp'))) {
          if (f.toLowerCase().includes(query.toLowerCase().replace(/^0x/, ''))) {
            const c = fs.readFileSync(path.join(overDir, f), 'utf8');
            const m = c.match(/RECOMP_FUNC\s+(\w+)/);
            results.push(`Override: ${f}${m ? ` → ${m[1]}` : ''}`);
          }
        }
      }
      for (const d of ['configs', 'config']) {
        const dir = path.join(PS2RECOMP_ROOT, d);
        if (!fs.existsSync(dir)) continue;
        for (const f of fs.readdirSync(dir, { recursive: true }).filter((f): f is string => typeof f === 'string' && f.endsWith('.toml'))) {
          const c = fs.readFileSync(path.join(dir, f), 'utf8');
          if (c.toLowerCase().includes(query.toLowerCase())) {
            const lines = c.split('\n').filter(l => l.toLowerCase().includes(query.toLowerCase()));
            results.push(`Config: ${f}`);
            lines.slice(0, 3).forEach(l => results.push(`  ${l.trim()}`));
          }
        }
      }
      return { content: [{ type: 'text' as const, text: results.length > 0 ? results.join('\n') : `No results for "${query}"` }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

server.tool('ps2recomp_list_overrides', 'List all PS2Recomp function overrides.',
  {},
  async () => {
    try {
      const dir = path.join(PS2RECOMP_ROOT, 'ps2xRuntime', 'src', 'lib', 'overrides');
      if (!fs.existsSync(dir)) return { content: [{ type: 'text' as const, text: 'Override dir not found' }], isError: true };
      const files = fs.readdirSync(dir).filter(f => f.endsWith('.cpp')).sort();
      const lines = files.map(f => { const m = f.match(/0x([0-9a-fA-F]+)/); return `  ${m ? '0x' + m[1] : '?'.padEnd(12)} ${f}`; });
      return { content: [{ type: 'text' as const, text: `${files.length} overrides:\n${lines.join('\n')}` }] };
    } catch (e: any) { return { content: [{ type: 'text' as const, text: `Error: ${e.message}` }], isError: true }; }
  }
);

// ==========================================================
//  MCP Resources
// ==========================================================
server.resource('ps2_memory_map', 'ps2://memory_map', async () => ({
  contents: [{ uri: 'ps2://memory_map', mimeType: 'text/plain', text: `PS2 EE Memory Map\n0x00000000-0x01FFFFFF  RDRAM (32MB)\n0x10000000-0x1000FFFF  EE Registers\n0x11000000-0x11FFFFFF  VU0/VU1\n0x12000000-0x12FFFFFF  GS Registers\n0x1C000000-0x1C3FFFFF  IOP RAM (2MB)\n0x1FC00000-0x1FFFFFFF  BIOS ROM (4MB)\n0x70000000-0x70003FFF  Scratchpad (16KB)` }]
}));

server.resource('debug_protocol', 'ps2://debug_protocol', async () => ({
  contents: [{ uri: 'ps2://debug_protocol', mimeType: 'text/plain', text: `PCSX2 Debug Server Protocol (port 21512)\nNewline-delimited JSON over TCP\n\nCommands: status, read_registers, write_register, set_pc, read_memory, write_memory, read_string, disassemble, evaluate, set_breakpoint, remove_breakpoint, list_breakpoints, set_memcheck, remove_memcheck, list_memchecks, pause, resume, step, step_over, get_threads, get_modules, is_valid_address, clear_breakpoints\n\nRequest:  {"cmd":"read_registers","cpu":"ee","category":0}\\n\nResponse: {"ok":true,"data":{...}}\\n` }]
}));

// ===== MAIN =====
async function main() {
  const transport = new StdioServerTransport();
  await server.connect(transport);
  console.error('PCSX2 ULTIMATE MCP Server v2.0 running');
  console.error(`PS2Recomp root: ${PS2RECOMP_ROOT}`);
}
main().catch(e => { console.error('Fatal:', e); process.exit(1); });

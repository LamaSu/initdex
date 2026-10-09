// Tests for docs/index.html: syntax-checks the inline module and unit-tests the pure helpers
// (between the ==pure-begin== / ==pure-end== markers) with Node >= 20. No dependencies.
// Usage: node tools/test_page.mjs
import { readFileSync, writeFileSync, mkdtempSync } from "node:fs";
import { createHash, randomBytes } from "node:crypto";
import { execFileSync } from "node:child_process";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import assert from "node:assert/strict";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const html = readFileSync(join(root, "docs", "index.html"), "utf8");
const scripts = [...html.matchAll(/<script type="module">([\s\S]*?)<\/script>/g)].map(m => m[1]);
assert.equal(scripts.length, 1, "expected exactly one inline module script");
const work = mkdtempSync(join(tmpdir(), "initdex-page-"));

// 1. The whole inline module parses as an ES module.
const modulePath = join(work, "page-module.mjs");
writeFileSync(modulePath, scripts[0]);
execFileSync(process.execPath, ["--check", modulePath]);
console.log("PASS  inline module passes node --check");

// 2. Pure helpers.
const afterBegin = scripts[0].split("// ==pure-begin==")[1];
assert.ok(afterBegin && afterBegin.includes("// ==pure-end=="), "pure block markers missing");
const pure = afterBegin.slice(afterBegin.indexOf("\n") + 1).split("// ==pure-end==")[0];
const purePath = join(work, "pure.mjs");
writeFileSync(purePath, pure + "\nexport { md5hex, hex, parsePartitions, layoutProblems, appImageProblem, otadataSlot, securityProblem, profileUrl, cleanHandle, cleanNames, utf8Trim, agentText, dexForAgent, BadgeLink, DEX_PROMPT, WANTED_PROMPT };\n");
const P = await import(pathToFileURL(purePath).href);
let passed = 0;
const test = async (name, fn) => { await fn(); passed++; console.log("PASS  " + name); };

await test("md5hex matches node crypto (edge sizes, random data, real firmware)", () => {
  const sizes = [0, 1, 3, 55, 56, 57, 63, 64, 65, 127, 128, 4096, 100003];
  const bufs = sizes.map(n => randomBytes(n));
  bufs.push(new Uint8Array(0x300000).fill(0xff));
  for (const f of ["initdex.bin", "otadata-ota1.bin", "otadata-ota0.bin"]) bufs.push(readFileSync(join(root, "docs", "firmware", f)));
  for (const b of bufs) assert.equal(P.md5hex(new Uint8Array(b)), createHash("md5").update(b).digest("hex"), `md5 mismatch for ${b.length} bytes`);
  assert.equal(P.md5hex(new TextEncoder().encode("abc")), "900150983cd24fb0d6963f7d28e17f72");
});

// Byte-exact builder for ESP-IDF tables; the conference/factory fixtures hash to the
// sector SHA-256s pinned by WorkOS's installer, so they equal real badges' tables.
const entry = (label, type, subtype, offset, size) => {
  const e = new Uint8Array(32), dv = new DataView(e.buffer);
  e.set([0xaa, 0x50, type, subtype]); dv.setUint32(4, offset, true); dv.setUint32(8, size, true);
  e.set(new TextEncoder().encode(label), 12);
  return e;
};
const table = (rows, { md5 = true, pad = 0x1000 } = {}) => {
  const out = new Uint8Array(pad).fill(0xff);
  rows.forEach((r, i) => out.set(entry(...r), i * 32));
  if (md5) {
    const n = rows.length * 32;
    out.set([0xeb, 0xeb], n);
    out.set(Buffer.from(createHash("md5").update(out.subarray(0, n)).digest()), n + 16);
  }
  return out;
};
const CONFERENCE = [["nvs", 1, 2, 0x9000, 0x5000], ["otadata", 1, 0, 0xe000, 0x2000], ["app0", 0, 0x10, 0x10000, 0x300000],
  ["app1", 0, 0x11, 0x310000, 0x300000], ["ffat", 1, 0x81, 0x610000, 0x9e0000], ["coredump", 1, 3, 0xff0000, 0x10000]];
const FACTORY = [["nvs", 1, 2, 0x9000, 0x4000], ["otadata", 1, 0, 0xd000, 0x2000], ["phy_init", 1, 1, 0xf000, 0x1000],
  ["ota_0", 0, 0x10, 0x20000, 0x4f0000], ["ota_1", 0, 0x11, 0x510000, 0x4f0000], ["storage", 1, 0x81, 0xa00000, 0x400000], ["coredump", 1, 3, 0xe00000, 0x10000]];
const verdict = bytes => { try { const { entries, md5ok } = P.parsePartitions(bytes); return P.layoutProblems(entries, md5ok).length ? "different" : "ok"; } catch { return "unreadable"; } };

await test("partition fixtures equal the real badge sectors (pinned SHA-256)", () => {
  assert.equal(createHash("sha256").update(table(CONFERENCE)).digest("hex"), "0bcf1787e46f4bf1ce9ad28bd22c2257e715987e913add5008c0265d3feb2fcd");
  assert.equal(createHash("sha256").update(table(FACTORY)).digest("hex"), "551de813efa412911f6d15afef5c0cdb9b3c2d9d8dece864280fa39963117e8f");
});
await test("layout check: accepts conference layout, refuses everything else", () => {
  assert.equal(verdict(table(CONFERENCE)), "ok");
  assert.equal(verdict(table(CONFERENCE, { md5: false })), "ok");
  assert.equal(verdict(table(CONFERENCE, { pad: 0xc00 })), "ok");
  assert.equal(verdict(table(FACTORY)), "different");
  assert.equal(verdict(table(CONFERENCE.map(r => r[0] === "app1" ? ["app1", 0, 0x11, 0x310000, 0x200000] : r))), "different");
  assert.equal(verdict(table(CONFERENCE.filter(r => r[0] !== "otadata"))), "different");
  assert.equal(verdict(table([...CONFERENCE.slice(0, 4), ["spill", 1, 0x81, 0x600000, 0x20000], ...CONFERENCE.slice(5)])), "different");
  const corrupt = table(CONFERENCE); corrupt[3 * 32 + 4] ^= 1;
  assert.equal(verdict(corrupt), "different");
  assert.equal(verdict(new Uint8Array(0x1000)), "unreadable");
  assert.equal(verdict(table(CONFERENCE, { md5: false }).subarray(0, 64)), "unreadable");
});
await test("firmware files: app image and otadata slot checks", () => {
  const app = new Uint8Array(readFileSync(join(root, "docs", "firmware", "initdex.bin")));
  const ota1 = new Uint8Array(readFileSync(join(root, "docs", "firmware", "otadata-ota1.bin")));
  const ota0 = new Uint8Array(readFileSync(join(root, "docs", "firmware", "otadata-ota0.bin")));
  assert.equal(P.appImageProblem(app), null);
  assert.notEqual(P.appImageProblem(ota1), null);
  assert.notEqual(P.appImageProblem(new Uint8Array(0x300001).fill(0xe9)), null);
  const s2 = app.slice(); s2[12] = 2; assert.notEqual(P.appImageProblem(s2), null);
  assert.equal(P.otadataSlot(ota1), 1);
  assert.equal(P.otadataSlot(ota0), 0);
  assert.equal(P.otadataSlot(new Uint8Array(0x2000).fill(0xff)), -1);
  assert.equal(P.otadataSlot(new Uint8Array(0x1000)), -1);
});
await test("security info check", () => {
  const info = (flags, crypt, chip) => { const b = new Uint8Array(20), dv = new DataView(b.buffer); dv.setUint32(0, flags, true); b[4] = crypt; dv.setUint32(12, chip, true); return b; };
  assert.equal(P.securityProblem(info(0, 0, 9)), null);
  assert.equal(P.securityProblem(info(0, 0b11, 9)), null);
  assert.notEqual(P.securityProblem(info(1, 0, 9)), null);
  assert.notEqual(P.securityProblem(info(4, 0, 9)), null);
  assert.notEqual(P.securityProblem(info(0, 1, 9)), null);
  assert.notEqual(P.securityProblem(info(0, 0, 5)), null);
  assert.notEqual(P.securityProblem(new Uint8Array(4)), null);
});
await test("profile links, handles, names", () => {
  assert.equal(P.profileUrl("github", "@octocat"), "https://github.com/octocat");
  assert.equal(P.profileUrl("X", "jack"), "https://x.com/jack");
  assert.equal(P.profileUrl("twitter", "jack"), "https://x.com/jack");
  assert.equal(P.profileUrl("linkedin", "in/jane-doe/"), "https://www.linkedin.com/in/jane-doe");
  assert.equal(P.profileUrl("huggingface", "julien-c"), "https://huggingface.co/julien-c");
  assert.equal(P.profileUrl("youtube", "@mkbhd"), "https://www.youtube.com/@mkbhd");
  assert.equal(P.profileUrl("url", "https://example.com"), null);
  assert.equal(P.profileUrl("github", "javascript:alert(1)/x"), null);
  assert.equal(P.profileUrl("github", ""), null);
  assert.equal(P.profileUrl("github", "a<b>"), "https://github.com/a%3Cb%3E");
  const { names, dropped } = P.cleanNames("1. Jane Doe\n- John Roe\n\n* jane doe\n  Ada Lovelace  \n" + Array.from({ length: 30 }, (_, i) => `Person ${i}`).join("\n"));
  assert.deepEqual(names.slice(0, 3), ["Jane Doe", "John Roe", "Ada Lovelace"]);
  assert.equal(names.length, 24);
  assert.equal(dropped, 9);
  assert.ok(new TextEncoder().encode(P.cleanNames("é".repeat(40)).names[0]).length <= 39);
});
await test("agent text: prompt + parseable JSON, no MAC addresses", () => {
  const dex = [{ name: "Jane Doe", company: "Acme", net: "github", handle: "@jane", mac: "AA:BB:CC:DD:EE:FF", t: 1760000000 },
    { name: "Bo", company: "", net: "url", handle: "https://bo.dev", mac: "11:22:33:44:55:66", t: 0 }];
  const text = P.agentText({ name: "Me", company: "Co", net: "x", handle: "me" }, dex);
  assert.ok(text.startsWith(P.DEX_PROMPT));
  assert.ok(!text.includes("AA:BB") && !text.includes("11:22"));
  const json = JSON.parse(text.split("```json\n")[1].split("\n```")[0]);
  assert.equal(json[0].profile, "https://github.com/jane");
  assert.equal(json[0].met_utc, "2025-10-09T08:53:20.000Z");
  assert.equal(json[1].profile, undefined);
  assert.ok(P.WANTED_PROMPT.includes("one name per line exactly as shown on their badge"));
});

// 3. BadgeLink against a simulated initdex badge (Web Serial port shape, Node web streams).
function fakeBadge({ silent = false } = {}) {
  let rx;
  const sent = [];
  const encode = s => new TextEncoder().encode(s);
  const port = {
    opened: false, closedCount: 0,
    async open() { this.opened = true; },
    async close() { this.closedCount++; },
    readable: new ReadableStream({ start(c) { rx = c; } }),
    writable: new WritableStream({
      write(chunk) {
        const cmd = JSON.parse(new TextDecoder().decode(chunk));
        sent.push(cmd);
        if (silent) return;
        // a log line, an event, a split reply: the client must pick out only the reply
        rx.enqueue(encode("I (1234) initdex: boot log\r\n{\"event\":\"wild\",\"name\":\"Ada\"}\n"));
        const reply = cmd.cmd === "hello" ? { ok: true, app: "initdex", ver: "0.1.0", me: { name: "Me" }, caught: 1, source: "init" }
          : cmd.cmd === "dex" ? { ok: true, dex: [{ name: "Jane", company: "Acme", net: "github", handle: "jane", mac: "AA", t: 1 }] }
          : cmd.cmd === "set_profile" && !cmd.name ? { ok: false, error: "name required" } : { ok: true, count: 2 };
        const s = JSON.stringify(reply) + "\n";
        rx.enqueue(encode(s.slice(0, 7)));
        setTimeout(() => rx.enqueue(encode(s.slice(7))), 5);
      },
    }),
  };
  return { port, sent, unplug: () => rx.error(new Error("device lost")) };
}
await test("BadgeLink: replies, events, errors, timeouts, disconnect", async () => {
  const events = [];
  let lost = 0;
  const b = fakeBadge();
  const link = new P.BadgeLink(b.port, e => events.push(e), () => lost++);
  await link.open();
  const [hello, dex] = await Promise.all([link.send({ cmd: "hello" }, 1000, m => "app" in m), link.send({ cmd: "dex" }, 1000, m => Array.isArray(m.dex))]);
  assert.equal(hello.app, "initdex");
  assert.equal(dex.dex[0].name, "Jane");
  assert.equal((await link.send({ cmd: "wanted", names: ["A", "B"] })).count, 2);
  await assert.rejects(link.send({ cmd: "set_profile", name: "" }), /name required/);
  assert.ok(events.length >= 3 && events.every(e => e.event === "wild"));
  assert.deepEqual(b.sent.map(c => c.cmd), ["hello", "dex", "wanted", "set_profile"]);
  const pending = link.send({ cmd: "hello" }, 5000, m => m.app === "never");
  setTimeout(() => b.unplug(), 30);
  await assert.rejects(pending, /disconnected/);
  assert.equal(lost, 1);
  await assert.rejects(link.send({ cmd: "hello" }), /disconnected/);

  const quiet = fakeBadge({ silent: true });
  const link2 = new P.BadgeLink(quiet.port);
  await link2.open();
  await assert.rejects(link2.send({ cmd: "hello" }, 50), /did not answer "hello"/);
  await link2.close();
  assert.equal(quiet.port.closedCount, 1);
  assert.equal(link2.closed, true);
});

console.log(`all ${passed + 1} checks passed`);

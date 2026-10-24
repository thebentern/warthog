// SPDX-License-Identifier: GPL-2.0-or-later
// The web flasher's own module script (web-flasher/index.html), run on a stubbed DOM, fetch, Web Serial
// and esptool-js: which images and region images each release offers, what doFlash hands esptool (the
// factory image at 0x0 and the chosen region image at 0x420000 in one write; Erase & flash refused
// before anything is erased when no country is chosen, because a full erase leaves the board on US),
// and the Configure tab's AT+REGION? parse, its preselect (stored before running) and Save.
// usage: node test_flasher.mjs <index.html> <scratch .mjs path>
import { readFileSync, writeFileSync } from "node:fs";
const [, , html, outMod] = process.argv;
const src = readFileSync(html, "utf8").match(/<script type="module">([\s\S]*?)<\/script>/)[1];
const ids = [...readFileSync(html, "utf8").matchAll(/id="([^"]+)"/g)].map((m) => m[1]);
let mod = src.replace(/^import \{ ESPLoader, Transport \} from .*$/m, "const { ESPLoader, Transport } = globalThis.__esptool;");
mod += `\nglobalThis.__t = { fillRegion, regionAssets, regionSource, populateVariants, populateRegions, doFlash, agnostic,
  setLoader: (l) => { loader = l; transport = { disconnect: async () => {} }; }, setWriter: (w) => { cwriter = w; }, feed: (s) => { cbuf += s; },
  REGNVS_OFFSET, REGNVS_SIZE };\n`;
writeFileSync(outMod, mod);

class El {
  constructor(id, tag) { this.id = id; this.tag = tag; this.children = []; this._sel = -1; this._v = ""; this.hidden = false; this.disabled = false;
    this.textContent = ""; this.files = null; this.listeners = {}; this.classList = { toggle() {} }; this.style = {}; this.dataset = {}; this.className = ""; }
  get value() { if (this.tag !== "select") return this._v; const o = this.children[this._sel]; return o ? o.value : ""; }
  set value(v) { if (this.tag !== "select") { this._v = v; return; } this._sel = this.children.findIndex((o) => o.value === String(v)); }
  get options() { return this.children; }
  set innerHTML(v) { this.children = []; this._sel = -1; for (const m of String(v).matchAll(/<option(?: value="([^"]*)")?>([^<]*)<\/option>/g)) this.appendChild(Object.assign(new El("", "option"), { value: m[1] ?? m[2], textContent: m[2] })); }
  appendChild(c) { this.children.push(c); if (this.tag === "select" && this._sel < 0 && !c.disabled) this._sel = this.children.length - 1; return c; }
  addEventListener(t, f) { (this.listeners[t] ||= []).push(f); }
  async fire(t) { for (const f of this.listeners[t] || []) await f(); }
}
const els = {};
const selects = new Set(["release", "variant", "regionPre", "region", "meshsec", "apChan"]);
for (const id of ids) els[id] = new El(id, selects.has(id) ? "select" : "div");
globalThis.document = { getElementById: (id) => els[id] || (els[id] = new El(id, "div")), querySelectorAll: () => [], createElement: (t) => new El("", t) };
globalThis.confirm = () => true;
const logLines = () => els.log.children.map((c) => c.textContent);

const factory = "x".repeat(2179184);
const asset = (name, size) => ({ name, size, browser_download_url: `https://dl/${name}` });
const variantAssets = (tag, keys) => keys.flatMap((k) => [asset(`warthog-${tag}-${k}.factory.bin`, 2179184), asset(`warthog-${tag}-${k}.bin`, 2100000)]);
const newRel = { tag_name: "v0.2.0", prerelease: false, draft: false,
  assets: [...variantAssets("v0.2.0", ["station", "mesh-smoke", "mesh-sae-meshvif", "mesh-sae-swccmp-meshvif"]),
           ...["AU", "JP", "KR", "NZ", "US"].map((c) => asset(`warthog-v0.2.0-region-${c}.bin`, 12288)), asset("flash.sh", 1500), asset("SHA256SUMS.txt", 3000)] };
const oldRel = { tag_name: "v0.1.0", prerelease: false, draft: false,
  assets: [...variantAssets("v0.1.0", ["au", "mesh-sae-meshvif", "mesh-sae-swccmp-meshvif", "mesh-smoke", "us"]), asset("flash.sh", 1356)] };
let regionBytes = 12288;
globalThis.fetch = async (url) => {
  if (url.startsWith("https://api.github.com/")) return { ok: true, json: async () => [newRel, oldRel] };
  const name = url.slice("https://dl/".length);
  const n = name.includes("-region-") ? regionBytes : 2179184;
  return { ok: true, arrayBuffer: async () => new Uint8Array(n).fill(name.includes("-region-") ? 0x41 : 0x42).buffer };
};
Object.defineProperty(globalThis, "navigator", { value: { serial: {} }, configurable: true });
globalThis.__esptool = { ESPLoader: class {}, Transport: class {} };

let fails = 0, oks = 0;
const check = (c, msg) => { if (c) { oks++; console.log("ok   " + msg); } else { fails++; console.log("FAIL " + msg); } };
await import(outMod);
const t = globalThis.__t;
await new Promise((r) => setTimeout(r, 20));
const optText = (id) => els[id].children.map((o) => `${o.value}=${o.textContent}`);

// Releases and variants.
check(els.release.value === "v0.2.0", "the newest release is selected first");
check(JSON.stringify(els.variant.children.map((o) => o.value)) === '["station","mesh-sae-meshvif","mesh-sae-swccmp-meshvif","mesh-smoke"]', "v0.2.0 lists station and the three mesh images");
check(els.variant.children.every((o) => !o.textContent.includes("US band")), "v0.2.0 mesh labels carry no (US band)");
check(!els.regionPreBox.hidden && JSON.stringify(els.regionPre.children.map((o) => o.value)) === '["","AU","JP","KR","NZ","US"]', "v0.2.0 offers keep + its five region images");
check(els.regionPre.value === "", "the region select starts on keep");
els.release.value = "v0.1.0"; await els.release.fire("change");
check(JSON.stringify(els.variant.children.map((o) => o.value)) === '["mesh-sae-meshvif","mesh-sae-swccmp-meshvif","mesh-smoke","us","au"]', "v0.1.0 lists its mesh images and the us and au images");
check(els.variant.children.filter((o) => o.value.startsWith("mesh")).every((o) => o.textContent.endsWith("(US band)")), "v0.1.0 mesh labels say (US band)");
check(els.regionPreBox.hidden, "v0.1.0 has no region images: the region select is hidden");
els.localBin.files = [{ name: "firmware.factory.bin", size: 2179184, arrayBuffer: async () => new Uint8Array(2179184).buffer }];
await els.localBin.fire("change");
check(!els.regionPreBox.hidden && t.regionSource()?.tag_name === "v0.2.0", "a local .bin takes region images from the newest release that has them");
els.localBin.files = null; await els.localBin.fire("change");

// doFlash: what reaches esptool.
const calls = [];
const loader = { eraseFlash: async () => calls.push("erase"), writeFlash: async (o) => calls.push(o.fileArray.map((f) => [f.address, f.data.length])) };
const flash = async (erase) => { calls.length = 0; els.log.children = []; t.setLoader(loader); await t.doFlash(erase); return JSON.stringify(calls); };
els.variant.value = "us";
check(await flash(false) === "[[[0,2179184]]]", "v0.1.0 us: the factory image alone at 0x0");
check(await flash(true) === '["erase",[[0,2179184]]]', "v0.1.0 us, erase: erase then the factory image (no region source)");
els.release.value = "v0.2.0"; await els.release.fire("change"); els.variant.value = "station";
check(await flash(false) === "[[[0,2179184]]]", "station, keep: the factory image alone");
check(logLines().some((l) => l.startsWith("No region written: the board keeps its stored region")), "station, keep: says the stored region is kept");
check(await flash(true) === "[]" && logLines().some((l) => l === "Erase & flash wipes the region: choose a country first."), "station, erase without a country: refused before anything is erased");
els.regionPre.value = "AU";
check(await flash(false) === `[[[0,2179184],[${0x420000},12288]]]`, "station, AU: factory at 0x0 and the 12K AU image at 0x420000 in one write");
check(logLines().includes("Region AU written."), "station, AU: says the region was written");
els.regionPre.value = "US";
check(await flash(true) === `["erase",[[0,2179184],[${0x420000},12288]]]`, "station, erase + US: erase, then factory and the US image");
regionBytes = 4096;
check(await flash(false) === "[]" && logLines().some((l) => l.includes("is 4096 bytes, not 12288")), "a region image of the wrong size is refused before writing");
regionBytes = 12288;

// Configure tab: AT+REGION? parsing.
const R = (ok, ...lines) => ({ ok, lines });
t.fillRegion(R(true, "+REGION: running=AU next_boot=AU stored=AU", "+REGION: allowed=AU,JP,KR,NZ,US default=US"));
check(els.region.value === "AU" && !els.regionSave.disabled && els.region.children.length === 5, "stored AU: AU preselected, Save enabled, five countries");
check(els.regionHint.textContent === "Stored in its own partition; reflashing keeps it. A board with none set runs as US.", "stored AU: the hint");
t.fillRegion(R(true, "+REGION: running=US next_boot=US stored=none", "+REGION: allowed=AU,JP,KR,NZ,US default=US"));
check(els.region.value === "US", "nothing stored: the running US preselected");
t.fillRegion(R(true, "+REGION: running=US next_boot=NZ stored=NZ", "+REGION: allowed=AU,JP,KR,NZ,US default=US"));
check(els.region.value === "NZ" && els.regionHint.textContent.includes("Running US; NZ after reboot."), "stored NZ, not yet applied: NZ preselected, the hint says after reboot");
t.fillRegion(R(true, "+REGION: running=none next_boot=none stored=invalid('u$')", "+REGION: allowed=AU,JP,KR,NZ,US default=US",
  "+REGION: radio held: the stored region is not a valid code; AT+REGION=<AU|JP|KR|NZ|US> then AT+RESET"));
check(els.region.value === "" && els.regionSave.disabled && els.region.children[0].disabled, "held, invalid: the placeholder, Save disabled");
check(els.regionHint.textContent.endsWith("Radio held: the stored region is not a valid code."), "held: the hint names the hold");
els.region.value = "JP"; await els.region.fire("change");
check(!els.regionSave.disabled, "a pick enables Save");
check(t.fillRegion(R(false, "+ERR: unknown command")) === "(region is fixed in this image: v0.1.0 per-region build)" && els.region.children.length === 0 && els.regionSave.disabled, "v0.1.0: no region select, Save disabled");
check(t.fillRegion(R(false, "(timeout — is the board booted normally, not in download mode?)")) === "(could not read AT+REGION?)", "a timeout is not reported as v0.1.0");

// Save sends AT+REGION=<CC>, then re-reads.
const sent = [];
t.setWriter({ write: async (b) => { const cmd = new TextDecoder().decode(b).trim(); sent.push(cmd);
  setTimeout(() => t.feed(cmd === "AT+REGION=KR" ? "+INFO: stored. AT+RESET to apply.\r\nOK\r\n"
    : "+REGION: running=US next_boot=KR stored=KR\r\n+REGION: allowed=AU,JP,KR,NZ,US default=US\r\nOK\r\n"), 5); } });
t.fillRegion(R(true, "+REGION: running=US next_boot=US stored=US", "+REGION: allowed=AU,JP,KR,NZ,US default=US"));
els.region.value = "KR"; await els.region.fire("change"); await els.regionSave.fire("click");
check(JSON.stringify(sent) === '["AT+REGION=KR","AT+REGION?"]', "Save sends AT+REGION=KR, then AT+REGION?");
check(els.cfgLog.children.some((c) => c.textContent === "Region: +INFO: stored. AT+RESET to apply."), "Save reports the +INFO line");
check(els.region.value === "KR" && els.regionHint.textContent.includes("Running US; KR after reboot."), "after Save: KR shown, next boot noted");
console.log(fails ? `FLASHER TESTS FAILED (${fails})` : `FLASHER TESTS PASSED (${oks})`);
process.exit(fails ? 1 : 0);

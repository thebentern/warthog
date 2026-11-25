// SPDX-License-Identifier: GPL-2.0-or-later
// The web flasher's own module script (web-flasher/index.html), run on a stubbed DOM, fetch, Web Serial
// and esptool-js: which images and region images each release offers, what doFlash hands esptool (the
// factory image at 0x0 and the chosen region image at 0x420000 in one write; Erase & flash refused
// before anything is erased when no country is chosen, because a full erase leaves the board on US),
// and the Configure tab's AT+REGION? parse, its preselect (stored before running) and Save,
// and the mesh Save's check for a running SAE mesh.
// usage: node test_flasher.mjs <index.html> <scratch .mjs path>
import { readFileSync, writeFileSync } from "node:fs";
const [, , html, outMod] = process.argv;
const page = readFileSync(html, "utf8");
const src = page.match(/<script type="module">([\s\S]*?)<\/script>/)[1];
const ids = [...page.matchAll(/id="([^"]+)"/g)].map((m) => m[1]);
let mod = src.replace(/^import \{ ESPLoader, Transport \} from .*$/m, "const { ESPLoader, Transport } = globalThis.__esptool;");
mod += `\nglobalThis.__t = { fillRegion, regionAssets, regionSource, populateVariants, populateRegions, doFlash, agnostic,
  setLoader: (l) => { loader = l; transport = { disconnect: async () => {} }; }, setWriter: (w) => { cwriter = w; }, feed: (s) => { cbuf += s; },
  REGNVS_OFFSET, REGNVS_SIZE };\n`;
writeFileSync(outMod, mod);

class El {
  constructor(id, tag) { this.id = id; this.tag = tag; this.children = []; this._sel = -1; this._v = ""; this.hidden = false; this.disabled = false;
    this.textContent = ""; this.files = null; this.listeners = {}; this.classList = { toggle() {} }; this.style = {}; this.dataset = {}; this.className = ""; }
  get value() { if (this.tag !== "select") return this._v; const o = this.children[this._sel]; return o ? o.value : ""; }
  set value(v) { if (this.tag !== "select") { this._v = v; if (this.id === "localBin" && v === "") this.files = null; return; } this._sel = this.children.findIndex((o) => o.value === String(v)); }
  get options() { return this.children; }
  set innerHTML(v) { this.children = []; this._sel = -1; for (const m of String(v).matchAll(/<option(?: value="([^"]*)")?>([^<]*)<\/option>/g)) this.appendChild(Object.assign(new El("", "option"), { value: m[1] ?? m[2], textContent: m[2] })); }
  appendChild(c) { this.children.push(c); if (this.tag === "select" && this._sel < 0 && !c.disabled) this._sel = this.children.length - 1; return c; }
  addEventListener(t, f) { (this.listeners[t] ||= []).push(f); }
  async fire(t) { for (const f of this.listeners[t] || []) await f(); }
}
const els = {};
const selects = new Set(["board", "release", "variant", "regionPre", "region", "meshsec", "apChan"]);
for (const id of ids) els[id] = new El(id, selects.has(id) ? "select" : "div");
els.board.innerHTML = page.match(/<select id="board">([\s\S]*?)<\/select>/)[1];
globalThis.document = { getElementById: (id) => els[id] || (els[id] = new El(id, "div")), querySelectorAll: () => [], createElement: (t) => new El("", t) };
globalThis.location = new URL("https://thebentern.github.io/warthog/");
globalThis.confirm = () => true;
const logLines = () => els.log.children.map((c) => c.textContent);

const factorySize = 2179184;
function factoryImage(flashHeader) {
  const bytes = new Uint8Array(factorySize);
  bytes.set([0xe9, 3, 2, flashHeader]);
  bytes.set([0xaa, 0x50], 0x8000);
  bytes[0x10000] = 0xe9;
  return bytes.buffer;
}
const asset = (name, size) => ({ name, size, browser_download_url: `https://dl/${name}` });
const variantAssets = (tag, keys) => keys.map((k) => asset(`warthog-${tag}-${k}.factory.bin`, factorySize));
const newRel = { tag_name: "v0.2.0", prerelease: false, draft: false,
  assets: [...variantAssets("v0.2.0", ["station", "mesh-smoke", "mesh-sae-meshvif", "mesh-sae-swccmp-meshvif", "heltec-rc32-68", "heltec-rc32-68-868"]),
           ...["AU", "CA", "EU", "JP", "KR", "NZ", "US"].map((c) => asset(`warthog-v0.2.0-region-${c}.bin`, 12288))] };
const oldRel = { tag_name: "v0.1.0", prerelease: false, draft: false,
  assets: [...variantAssets("v0.1.0", ["au", "mesh-sae-meshvif", "mesh-sae-swccmp-meshvif", "mesh-smoke", "us"]), asset("flash.sh", 1356)] };
let regionBytes = 12288;
const downloads = [];
globalThis.fetch = async (url) => {
  if (url.endsWith("/firmware/releases.json")) return { ok: true, json: async () => [newRel, oldRel] };
  if (!url.startsWith("https://thebentern.github.io/warthog/firmware/")) throw new Error(`unexpected download ${url}`);
  const name = url.split("/").pop();
  downloads.push(name);
  return { ok: true, arrayBuffer: async () => name.includes("-region-") ? new Uint8Array(regionBytes).buffer
    : factoryImage(name.includes("heltec-rc32-68") ? 0x4f : 0x3f) };
};
let bootMode, detectedKiB = 16384, disconnected = 0;
Object.defineProperty(globalThis, "navigator", { value: { serial: { async requestPort() {
  return { getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x1001 }) };
} } }, configurable: true });
globalThis.__esptool = {
  ESPLoader: class {
    async main(mode) { bootMode = mode; return "ESP32-S3"; }
    async getFlashSize() { return detectedKiB; }
  },
  Transport: class { async disconnect() { disconnected++; } },
};

let fails = 0, oks = 0;
const check = (c, msg) => { if (c) { oks++; console.log("ok   " + msg); } else { fails++; console.log("FAIL " + msg); } };
await import(outMod);
const t = globalThis.__t;
await new Promise((r) => setTimeout(r, 20));
const optText = (id) => els[id].children.map((o) => `${o.value}=${o.textContent}`);

// Releases and variants.
check(els.release.value === "v0.2.0", "the newest release is selected first");
check(JSON.stringify(els.variant.children.map((o) => o.value)) === '["station","mesh-sae-meshvif","mesh-sae-swccmp-meshvif","mesh-smoke"]', "v0.2.0 lists station and the three mesh images");
check(optText("variant").includes("mesh-sae-swccmp-meshvif=OpenMANET (batman)"), "build picker identifies the OpenMANET wizard build");
check(!els.regionPreBox.hidden && JSON.stringify(els.regionPre.children.map((o) => o.value)) === '["","AU","JP","KR","NZ","US"]', "v0.2.0 offers keep + its five region images");
check(els.regionPre.value === "", "the region select starts on keep");
els.release.value = "v0.1.0"; await els.release.fire("change");
check(JSON.stringify(els.variant.children.map((o) => o.value)) === '["mesh-sae-meshvif","mesh-sae-swccmp-meshvif","mesh-smoke","us","au"]', "v0.1.0 lists its mesh images and the us and au images");
check(els.buildInfo.textContent.includes("fixed US region") && els.variant.children.every((o) => o.textContent.length < 28), "v0.1.0 shows its mesh region in help instead of repeating it in the menu");
els.variant.value = "mesh-sae-swccmp-meshvif"; await els.variant.fire("change");
check(els.buildInfo.textContent.includes("LuCI mesh wizard") && els.buildInfo.textContent.includes("AT+MESHBATMAN=1") && els.buildInfo.textContent.includes("AT+SWCCMP=1"), "OpenMANET build explains the wizard setup and plain SAE alternative");
check(els.regionPreBox.hidden, "v0.1.0 has no region images: the region select is hidden");
els.localBin.files = [{ name: "firmware.factory.bin", size: factorySize, arrayBuffer: async () => factoryImage(0x3f) }];
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
check(await flash(false) === "[]" && logLines().some((l) => l.includes("is 4096 bytes, expected 12288")), "a region image of the wrong size is refused before writing");
regionBytes = 12288;

// Both RadioCore bands use the same ROM transport and different release and region assets.
els.board.value = "heltec-rc32-68"; await els.board.fire("change");
check(optText("release").length === 1 && optText("variant").join() === "heltec-rc32-68=Station", "915 MHz offers its own release image and hides older XIAO releases");
check(els.regionPre.children.map((o) => o.value).join() === ",AU,CA,JP,KR,NZ,US", "915 MHz offers Canada and filters out EU");
check(els.bootHold.textContent === "USER" && els.flashReset.textContent === "R", "RadioCore shows its own button sequence");
detectedKiB = 8192;
await els.connect.fire("click");
check(els.flash.disabled && disconnected === 1, "a hardware flash-size mismatch closes the port before allowing a write");
detectedKiB = 16384;
await els.connect.fire("click");
check(!els.flash.disabled && bootMode === "no_reset", "RadioCore connects via USB-Serial/JTAG in download mode");
els.regionPre.value = "CA";
check(await flash(false) === `[[[0,${factorySize}],[${0x420000},12288]]]` && downloads.at(-2).endsWith("heltec-rc32-68.factory.bin") && downloads.at(-1).endsWith("region-CA.bin"), "915 MHz flashes its station image and CA region from the mirror");
check(logLines().includes("Done. Tap R on the board."), "RadioCore prompts for the correct reboot button");

els.localBin.files = [{ name: "firmware.factory.bin", size: factorySize, arrayBuffer: async () => factoryImage(0x3f) }];
await els.localBin.fire("change");
check(await flash(true) === "[]" && logLines().some((l) => l.includes("16 MB flash")), "a XIAO image on the RadioCore profile is refused before erase");
els.board.value = "heltec-rc32-68-868"; await els.board.fire("change");
check(!els.localBin.files && els.regionPre.value === "" && els.regionPre.children.map((o) => o.value).join() === ",EU", "changing to 868 MHz clears the local image and only offers its EU region");
els.regionPre.value = "EU";
check(await flash(true) === `["erase",[[0,${factorySize}],[${0x420000},12288]]]` && downloads.at(-2).endsWith("heltec-rc32-68-868.factory.bin") && downloads.at(-1).endsWith("region-EU.bin"), "868 MHz erase and flash uses its station and EU images");
els.localBin.files = [{ name: "warthog-v0.2.0-heltec-rc32-68.factory.bin", size: factorySize, arrayBuffer: async () => factoryImage(0x4f) }];
await els.localBin.fire("change");
check(await flash(true) === "[]" && logLines().some((l) => l.includes("selected board and band")), "a named 915 MHz image is refused on the 868 MHz profile");
newRel.assets = newRel.assets.filter((a) => !a.name.includes("heltec-rc32-68-868"));
await els.board.fire("change");
check(els.release.disabled && !els.release.value, "no compatible release leaves the local RadioCore image option available");
els.localBin.files = [{ name: "firmware.factory.bin", size: factorySize, arrayBuffer: async () => factoryImage(0x4f) }];
await els.localBin.fire("change");
els.regionPre.value = "";
const beforeLocal = downloads.length;
check(await flash(false) === `[[[0,${factorySize}]]]` && downloads.length === beforeLocal, "a local RadioCore factory image flashes without downloading a release asset");

// Configure tab: AT+REGION? parsing.
const R = (ok, ...lines) => ({ ok, lines });
t.fillRegion(R(true, "+REGION: running=AU next_boot=AU stored=AU", "+REGION: allowed=AU,JP,KR,NZ,US default=US"));
check(els.region.value === "AU" && !els.regionSave.disabled && els.region.children.length === 5, "stored AU: AU preselected, Save enabled, five countries");
check(els.regionHint.textContent === "Stored in its own partition; reflashing keeps it. A board with none set runs as US.", "stored AU: the hint");
t.fillRegion(R(true, "+REGION: running=US next_boot=US stored=none", "+REGION: allowed=AU,JP,KR,NZ,US default=US"));
check(els.region.value === "US", "nothing stored: the running US preselected");
t.fillRegion(R(true, "+REGION: running=US next_boot=US stored=none", "+REGION: allowed=AU,CA,JP,KR,NZ,US default=US"));
check(els.region.children.some((o) => o.value === "CA" && o.textContent.startsWith("CA — Canada")), "915 MHz RadioCore: Canada appears when the board offers it");
t.fillRegion(R(true, "+REGION: running=EU next_boot=EU stored=none", "+REGION: allowed=EU default=EU"));
check(els.region.value === "EU" && els.region.children.length === 1 && els.regionHint.textContent.endsWith("runs as EU."), "868 MHz RadioCore: EU is the only choice and the reported default");
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
// Save mesh setting asks AT+MPMPEERS? first: a board running the SAE mesh is not sent AT+MESHSEC=
// (the setting does nothing there, and firmware v0.1.0 dropped every link when it was sent).
let sae = 1;
t.setWriter({ write: async (b) => { const cmd = new TextDecoder().decode(b).trim(); sent.push(cmd);
  setTimeout(() => t.feed(cmd === "AT+MPMPEERS?" ? `+MPMPEERS: self=4dc7f8 (none) sae=${sae} offers=0 sae_fail=0\r\nOK\r\n`
    : "+MESHSEC: open, stored, re-peering (~2s)\r\nOK\r\n"), 5); } });
els.meshsec.innerHTML = '<option value="0">Open</option><option value="1">Keyed</option>';
els.meshsec.value = "0";
sent.length = 0; els.cfgLog.children = [];
await els.meshsecSave.fire("click");
check(JSON.stringify(sent) === '["AT+MPMPEERS?"]' && els.cfgLog.children.some((c) => c.textContent.startsWith("Mesh: not sent")),
  "Save mesh setting on a board running the SAE mesh: AT+MESHSEC= is not sent, and the log says so");
sae = 0; sent.length = 0; els.cfgLog.children = [];
await els.meshsecSave.fire("click");
check(JSON.stringify(sent) === '["AT+MPMPEERS?","AT+MESHSEC=0"]' && els.cfgLog.children.some((c) => c.textContent === "Mesh: OK"),
  "Save mesh setting on a non-SAE board: AT+MESHSEC=0 is sent and reported");
// A board that never answers AT+MPMPEERS? (or answers ERROR) may be a v0.1.0 SAE mesh: nothing more is sent.
t.setWriter({ write: async (b) => { const cmd = new TextDecoder().decode(b).trim(); sent.push(cmd);
  if (cmd !== "AT+MPMPEERS?") setTimeout(() => t.feed("+MESHSEC: open, stored, re-peering (~2s)\r\nOK\r\n"), 5); } });
sent.length = 0; els.cfgLog.children = [];
await els.meshsecSave.fire("click");
check(JSON.stringify(sent) === '["AT+MPMPEERS?"]' && sent.length === 1, "Save mesh setting, AT+MPMPEERS? never answered: exactly one command is sent, no AT+MESHSEC=");
check(els.cfgLog.children.length === 1 && els.cfgLog.children[0].textContent === "Mesh: not sent. Could not read the mesh state." &&
  els.cfgLog.children[0].className === "err", "Save mesh setting, AT+MPMPEERS? never answered: one error line says the mesh state could not be read");
t.setWriter({ write: async (b) => { const cmd = new TextDecoder().decode(b).trim(); sent.push(cmd);
  setTimeout(() => t.feed(cmd === "AT+MPMPEERS?" ? "+ERR: unknown command\r\nERROR\r\n" : "+MESHSEC: open, stored, re-peering (~2s)\r\nOK\r\n"), 5); } });
sent.length = 0; els.cfgLog.children = [];
await els.meshsecSave.fire("click");
check(JSON.stringify(sent) === '["AT+MPMPEERS?"]' && els.cfgLog.children.some((c) => c.textContent === "Mesh: not sent. Could not read the mesh state."),
  "Save mesh setting, AT+MPMPEERS? answered ERROR: AT+MESHSEC= is not sent");
console.log(fails ? `FLASHER TESTS FAILED (${fails})` : `FLASHER TESTS PASSED (${oks})`);
process.exit(fails ? 1 : 0);

import { useState, useEffect, useCallback, useRef } from "react";

// ============================================================
// WAXWING IMAGE LAB
// Interactive prototype for the Waxwing micro-image format.
// Bayer 4x4 ordered dithering with three selectable color
// palettes, producing ~1 KB indexed PNGs at 128x128 pixels.
// ============================================================

// --- Bayer 4x4 ordered dithering matrix ---
const BAYER4 = [
  [0,8,2,10],[12,4,14,6],[3,11,1,9],[15,7,13,5]
];

const hex2rgb = h => [parseInt(h.slice(1,3),16), parseInt(h.slice(3,5),16), parseInt(h.slice(5,7),16)];

const PALETTES = {
  cedar: {
    name: "Cedar",
    sub: "Warm woodblock",
    desc: "Warm brown tones inspired by the cedar waxwing's plumage. Evokes woodblock prints and risograph art.",
    palette: ["#2D1F0B","#8B6914","#C4A35A","#F5F0E1"],
    bg: "#2D1F0B",
  },
  waxseal: {
    name: "Waxseal",
    sub: "Crimson stamp",
    desc: "Deep crimson tones drawn from the waxwing's signature red wing-tips. Bold and dramatic like a wax seal.",
    palette: ["#1A0505","#B22222","#D4836A","#F5F0E1"],
    bg: "#1A0505",
  },
  signal: {
    name: "Signal",
    sub: "Electric mesh",
    desc: "Cool teal tones representing the mesh network signal. Digital and modern with an analog soul.",
    palette: ["#0A0A0A","#1B3A2D","#2EC4B6","#CBF3F0"],
    bg: "#0A0A0A",
  },
};
const PALETTE_KEYS = Object.keys(PALETTES);

// ============================================================
// IMAGE PROCESSING
// ============================================================

function toGrayscale(data, len) {
  const g = new Float32Array(len);
  for (let i = 0; i < len; i++) {
    g[i] = (0.299*data[i*4] + 0.587*data[i*4+1] + 0.114*data[i*4+2]) / 255;
  }
  return g;
}

function adjustLevels(gray, contrast, brightness) {
  const out = new Float32Array(gray.length);
  for (let i = 0; i < gray.length; i++) {
    out[i] = Math.max(0, Math.min(1, (gray[i]-0.5)*contrast + 0.5 + brightness));
  }
  return out;
}

function ditherBayer4(gray, w, h) {
  const n = 4, max = 16, levels = 4;
  const out = new Uint8Array(w * h);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      const i = y*w+x;
      const t = (BAYER4[y%n][x%n] + 0.5) / max;
      out[i] = Math.max(0, Math.min(levels-1, Math.round(gray[i]*(levels-1) + (t-0.5))));
    }
  return out;
}

/** Process source image through Bayer 4x4 dither with a given palette */
function processImage(srcDataURL, paletteKey, sz, contrast, brightness) {
  return new Promise((resolve) => {
    const img = new Image();
    img.onload = () => {
      const off = document.createElement("canvas");
      off.width = sz; off.height = sz;
      const ctx = off.getContext("2d");
      const sW = img.naturalWidth || img.width;
      const sH = img.naturalHeight || img.height;
      const side = Math.min(sW, sH);
      ctx.drawImage(img, (sW-side)/2, (sH-side)/2, side, side, 0, 0, sz, sz);
      const srcPixels = ctx.getImageData(0, 0, sz, sz);

      const p = PALETTES[paletteKey];
      const len = sz * sz;
      let gray = toGrayscale(srcPixels.data, len);
      gray = adjustLevels(gray, contrast, brightness);

      const indices = ditherBayer4(gray, sz, sz);

      const colors = p.palette.map(hex2rgb);
      const out = new Uint8ClampedArray(len * 4);
      for (let i = 0; i < len; i++) {
        const [r,g,b] = colors[indices[i]];
        out[i*4]=r; out[i*4+1]=g; out[i*4+2]=b; out[i*4+3]=255;
      }
      ctx.putImageData(new ImageData(out, sz, sz), 0, 0);

      const dataURL = off.toDataURL("image/png");
      off.toBlob(blob => {
        resolve({ dataURL, pngSize: blob ? blob.size : 0 });
      }, "image/png");
    };
    img.onerror = () => resolve({ dataURL: "", pngSize: 0 });
    img.src = srcDataURL;
  });
}

/** Generate a sample gradient image as a data URL */
function generateSample() {
  const c = document.createElement("canvas");
  c.width = 512; c.height = 512;
  const ctx = c.getContext("2d");
  const g = ctx.createRadialGradient(256,200,30,256,256,300);
  g.addColorStop(0,"#ffffff"); g.addColorStop(0.25,"#ddccbb");
  g.addColorStop(0.5,"#998877"); g.addColorStop(0.75,"#554433"); g.addColorStop(1,"#110800");
  ctx.fillStyle = g; ctx.fillRect(0,0,512,512);
  ctx.fillStyle = "rgba(255,255,255,0.35)";
  ctx.beginPath(); ctx.arc(170,170,70,0,Math.PI*2); ctx.fill();
  ctx.fillStyle = "rgba(0,0,0,0.45)";
  ctx.beginPath(); ctx.arc(340,310,90,0,Math.PI*2); ctx.fill();
  ctx.fillStyle = "rgba(255,255,255,0.18)";
  ctx.fillRect(80,370,352,50);
  return c.toDataURL("image/png");
}

// ============================================================
// MAIN COMPONENT
// ============================================================

export default function WaxwingImageLab() {
  const [srcDataURL, setSrcDataURL] = useState(null);
  const [selectedPalette, setSelectedPalette] = useState("cedar");
  const [contrast, setContrast] = useState(1.15);
  const [brightness, setBrightness] = useState(0.0);
  const resolution = 128; // fixed output size
  const [caption, setCaption] = useState("");
  const [paletteOutputs, setPaletteOutputs] = useState({});
  const [tab, setTab] = useState("compose");
  const fileRef = useRef(null);

  // Generate sample on mount
  useEffect(() => {
    setSrcDataURL(generateSample());
  }, []);

  // Process all 3 palettes whenever source/params change
  useEffect(() => {
    if (!srcDataURL) return;
    let cancelled = false;
    async function run() {
      const outputs = {};
      for (const key of PALETTE_KEYS) {
        if (cancelled) return;
        outputs[key] = await processImage(srcDataURL, key, resolution, contrast, brightness);
      }
      if (!cancelled) setPaletteOutputs(outputs);
    }
    run();
    return () => { cancelled = true; };
  }, [srcDataURL, contrast, brightness, resolution]);

  const handleFile = useCallback(e => {
    const file = e.target.files?.[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = evt => setSrcDataURL(evt.target.result);
    reader.readAsDataURL(file);
  }, []);

  const handleDrop = useCallback(e => {
    e.preventDefault();
    const file = e.dataTransfer?.files?.[0];
    if (!file || !file.type.startsWith("image/")) return;
    const reader = new FileReader();
    reader.onload = evt => setSrcDataURL(evt.target.result);
    reader.readAsDataURL(file);
  }, []);

  const sel = PALETTES[selectedPalette];
  const selOut = paletteOutputs[selectedPalette];
  const selSize = selOut?.pngSize || 0;
  const estIndexed = selSize ? Math.round(selSize * 0.32) : 0;

  return (
    <div className="min-h-screen bg-neutral-950 text-neutral-100"
         style={{ fontFamily: "system-ui, sans-serif" }}>

      {/* Header */}
      <div className="border-b border-neutral-800 px-6 py-5">
        <div className="max-w-4xl mx-auto flex items-center gap-4">
          <WaxwingLogo size={40} palette={sel.palette} />
          <div>
            <h1 className="text-xl font-bold tracking-tight" style={{color: sel.palette[2]}}>
              Waxwing Image Lab
            </h1>
            <p className="text-xs text-neutral-500">
              Craft iconic micro-images for the mesh network
            </p>
          </div>
        </div>
      </div>

      <div className="max-w-4xl mx-auto px-4 py-6 space-y-8">

        {/* Upload */}
        <div onClick={() => fileRef.current?.click()}
             onDrop={handleDrop} onDragOver={e => e.preventDefault()}
             className="border-2 border-dashed border-neutral-700 rounded-xl p-4 text-center cursor-pointer hover:border-neutral-500 transition-colors">
          <p className="text-sm text-neutral-400">
            Drop a photo here or <span className="underline" style={{color:sel.palette[2]}}>browse</span>
          </p>
          <input ref={fileRef} type="file" accept="image/*" className="hidden" onChange={handleFile} />
        </div>

        {/* Tabs */}
        <div className="flex gap-1 bg-neutral-900 rounded-lg p-1 w-fit">
          {[["compose","Compose"],["grid","Grid Preview"]].map(([k,l]) => (
            <button key={k} onClick={() => setTab(k)}
              className={`px-4 py-1.5 rounded-md text-sm font-medium transition-colors ${
                tab===k ? "bg-neutral-700 text-white" : "text-neutral-400 hover:text-neutral-200"
              }`}>{l}</button>
          ))}
        </div>

        {tab === "compose" ? (
          <>
            {/* Palette picker label */}
            <div>
              <p className="text-xs text-neutral-500 uppercase tracking-widest mb-3">Choose a color palette</p>
            </div>

            {/* Palette cards */}
            <div className="grid grid-cols-3 gap-3">
              {PALETTE_KEYS.map(key => {
                const p = PALETTES[key];
                const out = paletteOutputs[key];
                const active = key === selectedPalette;
                return (
                  <button key={key} onClick={() => setSelectedPalette(key)}
                    className={`rounded-xl p-3 text-left transition-all border-2 ${
                      active ? "border-neutral-400 bg-neutral-800" : "border-transparent bg-neutral-900 hover:bg-neutral-850"
                    }`}>
                    <div className="flex gap-1.5 mb-2">
                      {p.palette.map((c,i) => (
                        <div key={i} className="w-4 h-4 rounded-sm" style={{backgroundColor:c}} />
                      ))}
                    </div>
                    {out?.dataURL ? (
                      <img src={out.dataURL} alt={p.name}
                           className="w-full aspect-square rounded-lg mb-2 bg-neutral-900"
                           style={{ imageRendering: "pixelated" }} />
                    ) : (
                      <div className="w-full aspect-square rounded-lg mb-2 bg-neutral-900 animate-pulse" />
                    )}
                    <p className="text-sm font-semibold">{p.name}</p>
                    <p className="text-xs text-neutral-500">{p.sub}</p>
                    {out && (
                      <p className="text-xs mt-1" style={{color: p.palette[2]}}>
                        ~{Math.round(out.pngSize * 0.32)} B indexed
                      </p>
                    )}
                  </button>
                );
              })}
            </div>

            {/* Large preview + controls */}
            <div className="grid grid-cols-1 md:grid-cols-2 gap-6">
              <div>
                <div className="rounded-xl overflow-hidden border border-neutral-800" style={{backgroundColor: sel.bg}}>
                  {selOut?.dataURL ? (
                    <img src={selOut.dataURL} alt="Preview"
                         className="w-full aspect-square" style={{ imageRendering: "pixelated" }} />
                  ) : (
                    <div className="w-full aspect-square bg-neutral-900 animate-pulse" />
                  )}
                </div>
                <div className="flex justify-between mt-2 text-xs text-neutral-500">
                  <span>{resolution}x{resolution}px &middot; Bayer 4x4</span>
                  {selSize > 0 && (
                    <span>
                      RGBA PNG: {(selSize/1024).toFixed(1)} KB
                      <span className="mx-1">&middot;</span>
                      <span style={{color: estIndexed <= 1024 ? "#4ade80" : "#f97316"}}>
                        Indexed: ~{estIndexed < 1024 ? `${estIndexed} B` : `${(estIndexed/1024).toFixed(1)} KB`}
                      </span>
                    </span>
                  )}
                </div>
              </div>

              <div className="space-y-5">
                <p className="text-sm text-neutral-400">{sel.desc}</p>

                <div>
                  <label className="text-xs font-medium text-neutral-400 mb-1 block">
                    Contrast ({contrast.toFixed(2)})
                  </label>
                  <input type="range" min="0.5" max="2.5" step="0.05"
                    value={contrast} onChange={e => setContrast(+e.target.value)}
                    className="w-full accent-neutral-500" />
                </div>

                <div>
                  <label className="text-xs font-medium text-neutral-400 mb-1 block">
                    Brightness ({brightness >= 0 ? "+" : ""}{brightness.toFixed(2)})
                  </label>
                  <input type="range" min="-0.4" max="0.4" step="0.02"
                    value={brightness} onChange={e => setBrightness(+e.target.value)}
                    className="w-full accent-neutral-500" />
                </div>

                <div>
                  <label className="text-xs font-medium text-neutral-400 mb-1 block">Caption</label>
                  <input type="text" value={caption} onChange={e => setCaption(e.target.value)}
                    placeholder="Add a caption..."
                    className="w-full bg-neutral-900 border border-neutral-700 rounded-lg px-3 py-2
                               text-sm text-neutral-200 placeholder-neutral-600 focus:outline-none
                               focus:border-neutral-500" />
                  <p className="text-xs text-neutral-600 mt-1">
                    Embedded as PNG tEXt metadata &middot; {caption.length}/140
                  </p>
                </div>

                <div className="bg-neutral-900 rounded-lg p-3 space-y-1.5">
                  <p className="text-xs font-medium text-neutral-400">Size Budget (1 KB target)</p>
                  <SizeBar label="Image" bytes={estIndexed} max={1024} color={sel.palette[2]} />
                  <SizeBar label="Caption" bytes={new TextEncoder().encode(caption).length + 20} max={1024} color={sel.palette[1]} />
                  <SizeBar label="GPS tag" bytes={32} max={1024} color={sel.palette[1]} />
                  <div className="border-t border-neutral-800 pt-1.5 flex justify-between text-xs">
                    <span className="text-neutral-400">Total estimated</span>
                    <span style={{color: sel.palette[2]}}>
                      {estIndexed + new TextEncoder().encode(caption).length + 52} B
                    </span>
                  </div>
                </div>
              </div>
            </div>
          </>
        ) : (
          <GridPreview outputs={paletteOutputs} paletteKey={selectedPalette} sel={sel} caption={caption} />
        )}

        {/* Actual-size strip */}
        <div className="border-t border-neutral-800 pt-6">
          <p className="text-xs text-neutral-500 mb-3 uppercase tracking-widest">Actual pixel size (1:1) &middot; same dither, three palettes</p>
          <div className="flex gap-4 items-end">
            {PALETTE_KEYS.map(key => {
              const p = PALETTES[key];
              const out = paletteOutputs[key];
              const active = key === selectedPalette;
              return (
                <div key={key} className="text-center">
                  {out?.dataURL ? (
                    <img src={out.dataURL} alt={p.name}
                         className={`border rounded ${active ? "border-neutral-400" : "border-neutral-800"}`}
                         style={{ width: resolution, height: resolution, imageRendering: "pixelated" }} />
                  ) : (
                    <div className="border border-neutral-800 rounded bg-neutral-900"
                         style={{ width: resolution, height: resolution }} />
                  )}
                  <p className={`text-xs mt-1 ${active ? "text-neutral-300" : "text-neutral-600"}`}>{p.name}</p>
                </div>
              );
            })}
          </div>
        </div>

        {/* Notes */}
        <div className="border-t border-neutral-800 pt-6 pb-10 text-xs text-neutral-600 space-y-2">
          <p><strong className="text-neutral-400">Dithering:</strong> Bayer 4x4 ordered dithering. Same algorithm, three color palettes.</p>
          <p><strong className="text-neutral-400">Format:</strong> Indexed 4-color PNG with embedded palette. Standard PNG viewable on any device.</p>
          <p><strong className="text-neutral-400">Metadata:</strong> Caption in PNG tEXt chunk, optional GPS as lat/lon. All EXIF stripped.</p>
          <p><strong className="text-neutral-400">Future:</strong> On-device OCR (Vision framework) for auto-captioning. Works fully offline.</p>
        </div>
      </div>
    </div>
  );
}

// ============================================================
// Sub-components
// ============================================================

function SizeBar({ label, bytes, max, color }) {
  const pct = Math.min(100, (bytes / max) * 100);
  return (
    <div className="flex items-center gap-2 text-xs">
      <span className="w-14 text-neutral-500">{label}</span>
      <div className="flex-1 h-2 bg-neutral-800 rounded-full overflow-hidden">
        <div className="h-full rounded-full transition-all" style={{width:`${pct}%`, backgroundColor:color}} />
      </div>
      <span className="w-12 text-right text-neutral-500">{bytes} B</span>
    </div>
  );
}

function GridPreview({ outputs, paletteKey, sel, caption }) {
  const out = outputs[paletteKey];
  const captions = [
    caption || "First light over the ridge",
    "Community board - farmers market Sat 8am",
    "Trail closed past mile marker 4",
    "Water source confirmed at junction",
    "Signal strong at summit cairn",
    "Wildflowers blooming early this year",
    "Cache restocked - batteries + snacks",
    "Sunset from the fire lookout",
    "New node installed at the bridge",
  ];

  return (
    <div>
      <p className="text-xs text-neutral-500 mb-3 uppercase tracking-widest">
        Feed view &middot; {sel.name} palette
      </p>
      <div className="grid grid-cols-3 gap-2">
        {captions.map((cap, i) => (
          <div key={i} className="rounded-lg overflow-hidden bg-neutral-900 border border-neutral-800 hover:border-neutral-600 transition-all cursor-pointer">
            {out?.dataURL ? (
              <img src={out.dataURL} alt="" className="w-full aspect-square"
                   style={{ imageRendering: "pixelated",
                            filter: i > 0 ? `brightness(${0.8+i*0.05}) contrast(${0.95+i*0.01})` : "none" }} />
            ) : (
              <div className="w-full aspect-square bg-neutral-900 animate-pulse" />
            )}
            <div className="px-2 py-1.5">
              <p className="text-xs text-neutral-400 truncate">{cap}</p>
              <p className="text-xs text-neutral-700 mt-0.5">{(i*3+1)}h ago</p>
            </div>
          </div>
        ))}
      </div>
    </div>
  );
}

// ============================================================
// WAXWING LOGO — pixel-art bird in the active palette
// ============================================================

function WaxwingLogo({ size = 40, palette }) {
  const ref = useRef(null);
  useEffect(() => {
    const c = ref.current;
    if (!c) return;
    c.width = size; c.height = size;
    const ctx = c.getContext("2d");
    const colors = palette.map(hex2rgb);
    const f = size / 20;
    ctx.scale(f, f);
    ctx.fillStyle = `rgb(${colors[0].join(",")})`;
    ctx.fillRect(0, 0, 20, 20);
    const rows = [
      "00000000000000000000",
      "00000000003300000000",
      "00000000033300000000",
      "00000000333300000000",
      "00000003333300000000",
      "00000033333300000000",
      "00000333333330000000",
      "00003333322333000000",
      "00033333322233300000",
      "00333333332223300000",
      "03333333333223300000",
      "33333333333322300000",
      "03333333333332330000",
      "00233333333333233000",
      "00023333333333322200",
      "00002333333333321100",
      "00000233333333210000",
      "00000022333221100000",
      "00000000222110000000",
      "00000000000000000000",
    ];
    rows.forEach((row, y) => {
      for (let x = 0; x < row.length; x++) {
        const ci = parseInt(row[x]);
        if (ci > 0) {
          const [r,g,b] = colors[ci];
          ctx.fillStyle = `rgb(${r},${g},${b})`;
          ctx.fillRect(x, y, 1, 1);
        }
      }
    });
    ctx.setTransform(1,0,0,1,0,0);
  }, [size, palette]);

  return (
    <canvas ref={ref} width={size} height={size}
      className="rounded-lg flex-shrink-0"
      style={{ width: size, height: size, imageRendering: "pixelated", backgroundColor: palette[0] }} />
  );
}

# Waxwing Companion — iOS

Native SwiftUI companion app for Waxwing mesh nodes. Scans for nearby Waxwing
BLE devices, connects, and displays node identity and status.

## Requirements

- **Mac** with macOS 15+ (Sequoia or later)
- **Xcode 26** (free from the Mac App Store)
- **Apple Account** (free — no paid Developer Program required for personal-device testing)
- **iPhone 16 Pro** running iOS 26.4, connected via USB (or on the same Wi-Fi for wireless deploy)

## Quick Start — Creating the Xcode Project

Since Xcode project files (`.xcodeproj`) are generated binary plists that are
hard to version-control, you'll create the project in Xcode and then drop the
source files in. This only takes a minute.

### 1. Install Xcode

Open the **App Store** on your Mac, search for **Xcode**, and install it.
First launch takes a while — it installs platform SDKs and simulators.

### 2. Create New Project

1. Open Xcode → **File → New → Project**
2. Choose **iOS → App** → Next
3. Fill in:
   - **Product Name:** `WaxwingCompanion`
   - **Team:** Select your Apple Account (see "Signing" below)
   - **Organization Identifier:** `com.waxwing` (or anything you like)
   - **Interface:** SwiftUI
   - **Language:** Swift
4. Save it somewhere convenient (e.g. your Desktop — you won't commit this)

### 3. Replace the Source Files

Xcode creates placeholder files. Replace them with the ones from this repo:

1. In Xcode's **Project Navigator** (left sidebar), delete the placeholder
   files: `ContentView.swift` and `WaxwingCompanionApp.swift` (choose
   "Move to Trash")
2. Drag the entire contents of **this repo's**
   `ios/WaxwingCompanion/WaxwingCompanion/` folder into Xcode's project
   navigator, dropping it onto the `WaxwingCompanion` group. When prompted:
   - Check **"Copy items if needed"**
   - Check **"Create groups"**
   - Make sure `WaxwingCompanion` target is checked

You should see this structure in Xcode:

```
WaxwingCompanion/
├── WaxwingCompanionApp.swift
├── BLE/
│   ├── BLEManager.swift
│   ├── CBORDecoder.swift
│   └── WaxwingConstants.swift
├── Models/
│   └── WaxwingNode.swift
└── Views/
    ├── ScannerView.swift
    └── NodeDetailView.swift
```

### 4. Add Bluetooth Permission

iOS requires a description of *why* your app uses Bluetooth. Without this, the
app will crash on launch.

1. Select the **WaxwingCompanion** project (blue icon) in the navigator
2. Select the **WaxwingCompanion** target
3. Go to the **Info** tab
4. Under **Custom iOS Target Properties**, click `+` and add:

| Key | Value |
|-----|-------|
| `NSBluetoothAlwaysUsageDescription` | `Waxwing Companion scans for and connects to nearby Waxwing mesh nodes over Bluetooth LE.` |

### 5. Signing (Free — No Developer Program Needed)

You do NOT need a paid Apple Developer Program ($99/yr) to run on your own
device. A free Apple Account works — apps just expire after 7 days and need
re-deploying.

1. In Xcode: **Xcode → Settings → Accounts** → click `+` → **Apple ID**
2. Sign in with your Apple Account
3. Back in the project: select the **WaxwingCompanion** target → **Signing & Capabilities** tab
4. Check **"Automatically manage signing"**
5. Select your **Personal Team** from the Team dropdown
6. The Bundle Identifier should auto-fill (e.g. `com.waxwing.WaxwingCompanion`)

### 6. Deploy to iPhone

1. Connect your iPhone 16 Pro via **USB** cable
2. **On your iPhone** (first time only):
   - Go to **Settings → Privacy & Security → Developer Mode** → toggle **ON** → restart
   - After restart, confirm when prompted
3. In Xcode, select your iPhone from the device dropdown (top center toolbar)
4. Press **Cmd+R** (or click the ▶ Play button) to build and run
5. **First deploy:** Xcode may say "Could not launch — untrusted developer"
   - On your iPhone: **Settings → General → VPN & Device Management**
   - Tap your Apple ID → **Trust**
   - Run again from Xcode

### 7. Use the App

1. Make sure your Pico W is powered on and running the Waxwing firmware
2. Open the app → tap **Scan** in the top right
3. Your node should appear as `WX:XXXXXXXX` in the list
4. Tap it to view details → tap **Connect**
5. The app reads the Device Identity characteristic and shows:
   - Protocol version and name
   - Transport Public Key and fingerprint
   - Capabilities (BLE Transfer, Unattended, etc.)
   - Manifest count and mode

## Wireless Debugging (Optional)

After the first USB deploy, you can deploy wirelessly:

1. Connect iPhone via USB
2. In Xcode: **Window → Devices and Simulators**
3. Select your iPhone → check **"Connect via network"**
4. Disconnect USB — your phone should still appear in the device dropdown
5. Now Cmd+R deploys over Wi-Fi (slower but convenient)

## Troubleshooting

**"No Waxwing nodes found"**
- Verify Pico W is powered and the LED is doing a slow heartbeat (1s blink)
- Bluetooth must be enabled on the iPhone
- The app only shows devices advertising the Waxwing service UUID — generic BLE devices won't appear

**App crashes on launch**
- Make sure you added `NSBluetoothAlwaysUsageDescription` in step 4

**"Untrusted Developer"**
- See step 6.5 above — trust your developer profile in iPhone Settings

**Build errors about missing files**
- Make sure all `.swift` files are added to the WaxwingCompanion target
  (select each file → check the target membership in the right sidebar)

**Free signing expires after 7 days**
- Just re-run from Xcode (Cmd+R) to re-deploy. This is a limitation of free accounts.

## Architecture

```
BLEManager (ObservableObject)
├── CoreBluetooth Central Manager
├── Scans for WaxwingUUID.service
├── Connects and discovers GATT services
└── Reads Device Identity → CBOR decode → DeviceIdentity struct

ScannerView
├── Scan/Stop button
├── Status bar (BT state, scan progress)
└── List of discovered WaxwingNode objects
    └── NavigationLink → NodeDetailView

NodeDetailView
├── Connection section (status, signal, connect/disconnect)
├── Device Identity section (name, TPK, fingerprint, protocol)
├── Capabilities section (flags decoded to human labels)
└── Mesh Status section (manifest count, attended/unattended mode)
```

## What's Next

- [ ] Manifest browsing (read + display file list)
- [ ] File transfer (request + receive chunks)
- [ ] Content viewing (text, images, audio)
- [ ] Rating submission
- [ ] Home node pairing (challenge-response via Device Config)

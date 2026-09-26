===============================================================================
  RNSBox  --  Reticulum Network Router  --  Quick Start
===============================================================================

This USB drive is shipped with your RNSBox router and contains the messaging
apps you need to start using the Reticulum network. The drive is READ-ONLY.

Open  index.html  in a web browser for a clickable version of this guide.

The exact version and SHA-256 of every file on this drive are listed in
SHA256SUMS.txt (the apps are the latest stable releases at build time).

-------------------------------------------------------------------------------
  WHAT IS THIS BOX?
-------------------------------------------------------------------------------
RNSBox runs "rnsd", the Reticulum Network Stack daemon, as a transport node.
When you plug it into a computer over USB-C, the computer gets a network
connection to the box, and through it, to the wider Reticulum network.

  - Admin web UI:   http://10.42.0.1/        (login: admin / admin)
  - The box on the network:
        AutoInterface      -> auto-discovered by apps on the USB link (easiest)
        TCPServerInterface -> 10.42.0.1 : 4242   (use this if you set up an
                                                  interface by hand)

-------------------------------------------------------------------------------
  PICK AN APP   (each folder holds installers for every platform)
-------------------------------------------------------------------------------

  \MeshChat\    Reticulum MeshChat -- easiest, recommended start. Windows
                installer or portable .exe, macOS .dmg, Linux .AppImage.

  \MeshChatX\   MeshChatX -- feature-rich MeshChat fork (RNS + LXMF + LXST in
                one app). Windows, macOS, Linux (AppImage/deb/rpm/flatpak),
                Android APK.

  \Ratspeak\    Account-free LXMF messenger with voice (experimental) + games.
                Windows .exe/.msi, macOS .dmg, Linux AppImage/.deb/.rpm,
                Android APKs. NOTE: alpha software; the macOS build is
                unsigned. AGPL-3.0 (source at the project page below).

  \Columba\     Native Android client (Material Design). Install the APK that
                matches your phone (arm64-v8a for most modern phones, or the
                universal APK).

  \Sideband\    Advanced client (voice calls, telemetry). Windows .zip, Linux
                AppImage, Android APK. NOTE: licensed CC BY-NC-SA 4.0
                (NonCommercial).

ON THE BOX ITSELF
  NomadNet (terminal LXMF + pages client) is already installed on RNSBox.
  SSH in (root / admin) and run:  nomadnet
  (It is disabled as a background service by default; running it interactively
   is fine.)

-------------------------------------------------------------------------------
  CONNECTING AN APP TO THIS BOX
-------------------------------------------------------------------------------
1. Plug RNSBox into your computer with USB-C. Your OS gets a new network
   connection (the box is 10.42.0.1).
2. Open the app. Most will AUTO-DISCOVER the box via the AutoInterface on the
   USB link -- nothing to configure.
3. If you need to add it by hand, add a TCP Client interface pointing at:
        Host: 10.42.0.1     Port: 4242
4. You're on Reticulum. Find a peer's address and start messaging.

-------------------------------------------------------------------------------
  APPS & LICENSES
-------------------------------------------------------------------------------
  MeshChat    (MIT)              github.com/liamcottle/reticulum-meshchat
  MeshChatX   (0BSD + MIT)       github.com/Quad4-Software/MeshChatX
  Ratspeak    (AGPL-3.0, alpha)  github.com/ratspeak/Ratspeak
  Columba     (MPL-2.0)          github.com/torlando-tech/columba
  Sideband    (CC BY-NC-SA 4.0)  github.com/markqvist/Sideband
  NomadNet    (on the device)    github.com/markqvist/NomadNet

  Exact versions + SHA-256 for every file are in  SHA256SUMS.txt.
  For newer versions, visit the project pages above.
===============================================================================

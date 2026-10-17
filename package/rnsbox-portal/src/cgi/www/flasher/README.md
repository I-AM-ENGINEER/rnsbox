# RNode Flasher — embedded copy

Mirror of https://flasher.rns.moscow/ (taken 2026-10-17), served by the
box's web server at /flasher/. Asset paths were flattened to js/*.js;
nothing else is modified. No firmware images ship with the box — flashing
stays a visit to the original site with the firmware downloaded there.

Why a local copy: display-less RNodes (T-Echo, RAK4631) can only surface
their BLE pairing code through the flasher UI, and the bench may be
offline. Served over plain HTTP the page detects the non-secure context on
its own and switches to the WebSocket Configurator: enter the BOX's
address (10.42.0.1) as the host and it speaks KISS to the RNode through
the box's bridge (rnodebridge, ws port 7634, USB serial or BLE). Web
Bluetooth / Web Serial modes of the page need a secure context and stay
available on the original https site.

# Showcase beta 3-1 build pack

This pack builds the rootless Showcase package on your iPhone. It does not install the package, stop Bluetooth, or change your current Showcase app.

1. Extract the ZIP anywhere under `/var/mobile`.
2. Open a root shell and enter the extracted folder.
3. Run `chmod +x build.sh`.
4. Run `sudo ./build.sh`.
5. Send Amine the `.deb` from the new `output` folder.

The build uses the SDK and compiler already installed on your phone. If it fails, send `build.log` instead.

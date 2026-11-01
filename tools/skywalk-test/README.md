# AppleConvergedIPC implementation test

This kit validates the new Showcase BTstack backend on a rootless device that
publishes `hci` and `acl` through AppleConvergedIPC Skywalk interfaces.

It builds everything on the target device. Before installing anything, it
first requires a direct HCI Reset response, then starts the candidate BTstack
daemon and requires it to bring the controller to the working state. Only a
passing daemon gate installs `Showcase Skywalk Test` beside the normal
Showcase app. The package database, normal app bundle, and production BTdaemon
are not changed.

Run the generated kit as root:

```sh
sudo /bin/sh ./run.sh
```

If the script reports that the controller gate passed, open **Showcase Skywalk
Test**, complete one wireless CarPlay connection, stop it, and run the
collection command printed by the script. If a gate fails, send the private
archive printed immediately by `run.sh`; no app run is needed.

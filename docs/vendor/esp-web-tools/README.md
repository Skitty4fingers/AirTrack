# Vendored esp-web-tools

`esp-web-tools` 10.4.0 (Apache-2.0, https://github.com/esphome/esp-web-tools),
the contents of the published package's `dist/web/` directory, unmodified.

It is vendored rather than loaded from a CDN so `../../flash.html` keeps
working without third-party requests. The bundle is self-contained: the only
external URLs in it are links to USB-bridge driver downloads shown in its own
error dialogs.

To update: download the package tarball from the npm registry and copy
`package/dist/web/*.js` plus `package/LICENSE` here.

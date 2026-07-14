forge.min.js.gz — node-forge v1.3.1 (https://github.com/digitalbazaar/forge),
BSD-3-Clause license. Fetched from
https://cdn.jsdelivr.net/npm/node-forge@1.3.1/dist/forge.min.js and gzipped
unmodified (gzip -9). Embedded into the firmware (EMBED_FILES) and served at
/forge.min.js with Content-Encoding: gzip. Used ONLY by the web UI's LoTW
certificate import dialog to parse the user's TQSL .p12 in the BROWSER
(supports the legacy RC2-40/3DES and modern AES/PBES2 PKCS#12 variants);
the extracted cert + key DER are then POSTed to /api/lotw_cert. The p12
passphrase itself never reaches the device.

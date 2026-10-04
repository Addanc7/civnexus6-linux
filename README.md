# civnexus6-linux

Native Linux CivNexus6 and Civ VI asset cooker, packaged as an AppImage.

Download: https://github.com/Addanc7/civnexus6-linux/releases/download/v1.0.0/CivNexus6-linux-x86_64.AppImage

```bash
chmod +x CivNexus6-linux-x86_64.AppImage
./CivNexus6-linux-x86_64.AppImage
./CivNexus6-linux-x86_64.AppImage cook --mode XLP --platform Windows \
  --pantry /path/to/pantry --config Civ6.cfg file.xlp
```

Point `--pantry` at your real Civ VI pantry. The pantry is not inside the image.

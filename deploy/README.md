# AVF - AV Filter Deployment Package

## Quick Start

1. **Setup Test Certificate** (first time only):
   ```
   Setup-TestCert.bat
   ```
   This will:
   - Enable test signing mode (requires reboot)
   - Create a self-signed code signing certificate
   - Sign the avf.sys driver

2. **Reboot** if prompted (required after enabling test signing)

3. **Install the driver** - Double-click:
   ```
   Install.bat
   ```
   Or run in Admin PowerShell:
   ```powershell
   .\Install-AVF.ps1
   ```

4. **Run the monitor**:
   ```
   avf.exe [file1] [file2] ...
   ```
   - With file arguments: Only monitor those specific files
   - Without arguments: Monitor ALL file access (verbose!)

5. **(Optional) Start a Security Consultant** before running avf.exe to enable blocking.
   See `SECURITY_CONSULTANT_PROTOCOL.md` for details.

## Files

| File | Description |
|------|-------------|
| `avf.sys` | Kernel minifilter driver |
| `avf.exe` | User-mode monitor application |
| `avf.inf` | Driver installation information |
| `Setup-TestCert.bat` | **Run first** - Creates test signing certificate |
| `Setup-TestCert.ps1` | PowerShell certificate setup script |
| `Install.bat` | Double-click to install driver |
| `Uninstall.bat` | Double-click to uninstall |
| `Install-AVF.ps1` | PowerShell installer script |
| `Uninstall-AVF.ps1` | PowerShell uninstaller script |
| `SECURITY_CONSULTANT_PROTOCOL.md` | IPC protocol documentation |

## Usage Examples

### Monitor specific files
```cmd
avf.exe C:\important\secret.docx C:\data\config.ini
```

### Monitor all file access (noisy!)
```cmd
avf.exe
```

### With Security Consultant (Python example)
1. Start the consultant first (see protocol docs)
2. Then start avf.exe
3. Consultant can now ALLOW or BLOCK file operations

## Uninstalling

Double-click `Uninstall.bat` or run:
```powershell
.\Install-AVF.ps1 -Uninstall
```

## Troubleshooting

### "Driver failed to load"
- Make sure test signing is enabled: `bcdedit /enum | findstr testsigning`
- Reboot after enabling test signing

### "Access Denied"
- Run Install.bat as Administrator
- Right-click ? "Run as administrator"

### Driver won't unload
- Close avf.exe first
- Make sure no handles are open to monitored files
- Try: `fltmc unload avf`

## Requirements

- Windows 10/11 x64
- Test signing mode enabled (for unsigned driver)
- Administrator privileges for install/uninstall

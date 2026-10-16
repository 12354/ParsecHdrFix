TL;NOSLOP;
Parsec doesnt support HDR. Stream with HDR looks bad. This converts HDR=>SDR before streaming it to clients.

Put bcrypt.dll next to parsecd.exe:
C:\Program Files\Parsec\bcrypt.dll

Capture order on an HDR display: DXGI duplication FP16 (like OBS), then borderless Windows Graphics Capture FP16, then Parsec's own capture untouched.

Still looks wrong? Send this file:
C:\ProgramData\ParsecHdrShim\hdr-shim.log

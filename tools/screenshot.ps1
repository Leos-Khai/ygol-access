# Captures the primary screen to the given path (what is really shown, overlays included).
# Usage: powershell -File tools/screenshot.ps1 out.png
param([string]$Out = "shot.png")
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
[System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
$bmp.Save([IO.Path]::GetFullPath($Out))
Write-Output "saved $Out"

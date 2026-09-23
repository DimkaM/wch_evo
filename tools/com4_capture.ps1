param(
    [string]$Port = 'COM4',
    [int]$Baud = 115200,
    [int]$Seconds = 20,
    [string]$OutFile = "$env:TEMP\com4_capture.txt"
)

# read-only serial capture used for the hardware bring-up gates of this project
$sp = New-Object System.IO.Ports.SerialPort($Port, $Baud, 'None', 8, 'One')
$sp.ReadTimeout = 200
Set-Content -Path $OutFile -Value ''
try { $sp.Open() } catch { Add-Content -Path $OutFile -Value ('OPEN FAILED: ' + $_.Exception.Message) }
$n = 0
if ($sp.IsOpen) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
        try { $l = $sp.ReadLine(); if ($l) { $n++; Add-Content -Path $OutFile -Value $l.Trim() } } catch { }
    }
    $sp.Close()
}
Add-Content -Path $OutFile -Value ("TOTAL LINES=$n")

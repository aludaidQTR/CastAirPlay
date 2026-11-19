param(
    [ValidateRange(1, 65535)]
    [int]$Port = 47800
)

$ErrorActionPreference = 'Stop'
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
$listener.Start()

try {
    Write-Host "Listening on 127.0.0.1:$Port; start the Android probe now."
    $client = $listener.AcceptTcpClient()
    try {
        $stream = $client.GetStream()
        $stream.ReadTimeout = 10000
        $bytes = [System.Collections.Generic.List[byte]]::new()
        while ($bytes.Count -lt 256) {
            $value = $stream.ReadByte()
            if ($value -lt 0) { throw 'Phone closed the connection before sending a complete line.' }
            if ($value -eq 10) { break }
            if ($value -ne 13) { $bytes.Add([byte]$value) }
        }

        $line = [System.Text.Encoding]::UTF8.GetString($bytes.ToArray())
        Write-Host "Received: $line"
        if ($line -ne 'AICAM_USB_PROBE Android') {
            throw "Unexpected probe payload: $line"
        }

        $ack = [System.Text.Encoding]::UTF8.GetBytes("ACK Android`n")
        $stream.Write($ack, 0, $ack.Length)
        $stream.Flush()
        Write-Host 'PASS'
    }
    finally {
        $client.Dispose()
    }
}
finally {
    $listener.Stop()
}

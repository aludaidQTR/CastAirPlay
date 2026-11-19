using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Xml;
using System.Xml.Linq;

const int UsbmuxPort = 27015;
const int DevicePort = 47800;
const int MaxPacketLength = 1024 * 1024;
const string ProbeLine = "AICAM_USB_PROBE iOS\n";

try
{
    uint? deviceId = null;
    var deadline = DateTime.UtcNow.AddSeconds(60);
    while (deviceId is null && DateTime.UtcNow < deadline)
    {
        deviceId = await FindFirstDeviceAsync();
        if (deviceId is null)
            await Task.Delay(TimeSpan.FromSeconds(1));
    }

    if (deviceId is null)
        throw new TimeoutException("No usbmuxd device appeared within 60 seconds.");

    Console.WriteLine($"Attached device {deviceId.Value}");
    using var socket = new TcpClient();
    socket.ReceiveTimeout = 10000;
    socket.SendTimeout = 10000;
    await socket.ConnectAsync(IPAddress.Loopback, UsbmuxPort);
    using var stream = socket.GetStream();
    await SendPlistAsync(stream, 2, CreateMessage("Connect", new Dictionary<string, object>
    {
        ["DeviceID"] = deviceId.Value,
        ["PortNumber"] = HostToNetworkPort(DevicePort)
    }));

    var connectReply = await ReadPacketAsync(stream);
    EnsurePacketResult(connectReply, 2);
    Console.WriteLine($"Connected to device port {DevicePort}");

    var payload = Encoding.UTF8.GetBytes(ProbeLine);
    await stream.WriteAsync(payload);
    await stream.FlushAsync();

    var response = await ReadLineAsync(stream, 256);
    Console.WriteLine($"Received: {response}");
    if (response != "ACK iOS")
        throw new InvalidDataException($"Unexpected response: {response}");

    Console.WriteLine("PASS");
    return 0;
}
catch (Exception exception)
{
    Console.Error.WriteLine($"FAIL: {exception.Message}");
    return 1;
}

static async Task<uint?> FindFirstDeviceAsync()
{
    using var socket = new TcpClient();
    socket.ReceiveTimeout = 5000;
    socket.SendTimeout = 5000;
    await socket.ConnectAsync(IPAddress.Loopback, UsbmuxPort);
    using var stream = socket.GetStream();
    await SendPlistAsync(stream, 1, CreateMessage("ListDevices"));
    var packet = await ReadPacketAsync(stream);
    EnsurePacketResult(packet, 1);
    var dictionary = ParsePlist(packet.Payload).Root?.Element("dict");
    if (dictionary is null)
        throw new InvalidDataException("usbmuxd returned an invalid plist dictionary.");

    var devices = FindKey(dictionary, "DeviceList");
    if (devices?.Name != "array")
        return null;

    foreach (var device in devices.Elements("dict"))
    {
        var properties = FindKey(device, "Properties");
        if (properties is null || properties.Name != "dict")
            properties = device;

        var id = FindKey(properties, "DeviceID");
        if (id?.Name == "integer" && uint.TryParse(id.Value, out var value))
            return value;
    }

    return null;
}

static XDocument CreateMessage(string messageType, IReadOnlyDictionary<string, object>? fields = null)
{
    var entries = new List<object>
    {
        new XElement("key", "MessageType"),
        new XElement("string", messageType),
        new XElement("key", "ClientVersionString"),
        new XElement("string", "CastAirPlay AiCam USB probe"),
        new XElement("key", "ProgName"),
        new XElement("string", "AiCamUsbProbe"),
        new XElement("key", "kLibUSBMuxVersion"),
        new XElement("integer", "3")
    };

    if (fields is not null)
    {
        foreach (var (key, value) in fields)
        {
            entries.Add(new XElement("key", key));
            entries.Add(new XElement("integer", Convert.ToString(value, System.Globalization.CultureInfo.InvariantCulture)));
        }
    }

    return new XDocument(
        new XDeclaration("1.0", "UTF-8", null),
        new XElement("plist", new XAttribute("version", "1"), new XElement("dict", entries)));
}

static async Task SendPlistAsync(NetworkStream stream, uint tag, XDocument document)
{
    var payload = Encoding.UTF8.GetBytes(document.ToString(SaveOptions.DisableFormatting));
    var packet = new byte[16 + payload.Length];
    BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(0, 4), (uint)packet.Length);
    BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(4, 4), 1);
    BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(8, 4), 8);
    BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(12, 4), tag);
    payload.CopyTo(packet.AsSpan(16));
    await stream.WriteAsync(packet);
    await stream.FlushAsync();
}

static async Task<(uint MessageType, uint Tag, byte[] Payload)> ReadPacketAsync(NetworkStream stream)
{
    var header = new byte[16];
    await ReadExactlyAsync(stream, header);
    var length = BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(0, 4));
    if (length < 16 || length > MaxPacketLength)
        throw new InvalidDataException($"Invalid usbmuxd packet length: {length}.");

    var payload = new byte[length - 16];
    await ReadExactlyAsync(stream, payload);
    var version = BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(4, 4));
    if (version != 1)
        throw new InvalidDataException($"Unexpected usbmuxd protocol version: {version}.");

    return (
        BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(8, 4)),
        BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(12, 4)),
        payload);
}

static async Task ReadExactlyAsync(NetworkStream stream, Memory<byte> buffer)
{
    var read = 0;
    while (read < buffer.Length)
    {
        var count = await stream.ReadAsync(buffer[read..]);
        if (count == 0)
            throw new EndOfStreamException("usbmuxd closed the connection.");
        read += count;
    }
}

static XDocument ParsePlist(byte[] payload)
{
    using var input = new MemoryStream(payload);
    using var reader = XmlReader.Create(input, new XmlReaderSettings
    {
        DtdProcessing = DtdProcessing.Ignore,
        XmlResolver = null,
        MaxCharactersInDocument = MaxPacketLength
    });
    return XDocument.Load(reader);
}

static void EnsurePacketResult((uint MessageType, uint Tag, byte[] Payload) packet, uint expectedTag)
{
    if (packet.MessageType != 8 || packet.Tag != expectedTag)
        throw new InvalidDataException($"Unexpected usbmuxd reply (type {packet.MessageType}, tag {packet.Tag}).");
    EnsureResultDictionary(ParsePlist(packet.Payload).Root?.Element("dict"));
}

static void EnsureResultDictionary(XElement? dictionary)
{
    if (dictionary is null)
        throw new InvalidDataException("usbmuxd returned an invalid plist dictionary.");
    var message = FindKey(dictionary, "MessageType")?.Value;
    if (message != "Result")
        throw new InvalidDataException($"Expected usbmuxd Result, received {message ?? "unknown"}.");
    var result = FindKey(dictionary, "Number")?.Value;
    if (result != "0")
        throw new InvalidOperationException($"usbmuxd rejected request (result {result ?? "missing"}).");
}

static XElement? FindKey(XElement dictionary, string key)
{
    var elements = dictionary.Elements().ToArray();
    for (var index = 0; index + 1 < elements.Length; index += 2)
    {
        if (elements[index].Name == "key" && elements[index].Value == key)
            return elements[index + 1];
    }
    return null;
}

static ushort HostToNetworkPort(int port) =>
    unchecked((ushort)IPAddress.HostToNetworkOrder((short)port));

static async Task<string> ReadLineAsync(NetworkStream stream, int maximumLength)
{
    var bytes = new List<byte>();
    while (bytes.Count < maximumLength)
    {
        var one = new byte[1];
        await ReadExactlyAsync(stream, one);
        if (one[0] == (byte)'\n')
            return Encoding.UTF8.GetString(bytes.ToArray()).TrimEnd('\r');
        bytes.Add(one[0]);
    }
    throw new InvalidDataException("Phone response exceeded the line length limit.");
}

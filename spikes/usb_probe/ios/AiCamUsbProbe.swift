import Network
import SwiftUI

@main
struct AiCamUsbProbeApp: App {
    var body: some Scene {
        WindowGroup {
            ProbeView()
        }
    }
}

@MainActor
final class Probe: ObservableObject {
    @Published private(set) var status = "Starting listener..."
    private var listener: NWListener?

    func start() {
        do {
            let listener = try NWListener(using: .tcp, on: 47800)
            listener.newConnectionHandler = { [weak self] connection in
                self?.handle(connection)
            }
            listener.stateUpdateHandler = { [weak self] state in
                Task { @MainActor in
                    switch state {
                    case .ready:
                        self?.status = "Listening on TCP 47800"
                    case .failed(let error):
                        self?.status = "Listener failed: \(error)"
                    default:
                        break
                    }
                }
            }
            listener.start(queue: .main)
            self.listener = listener
        } catch {
            status = "Listener failed: \(error)"
        }
    }

    private func handle(_ connection: NWConnection) {
        connection.start(queue: .main)
        connection.receive(minimumIncompleteLength: 1, maximumLength: 256) { [weak self] data, _, _, error in
            guard let self else { return }
            guard error == nil, let data,
                  let line = String(data: data, encoding: .utf8),
                  line == "AICAM_USB_PROBE iOS\n" else {
                Task { @MainActor in self.status = "FAIL: invalid probe payload" }
                connection.cancel()
                return
            }
            connection.send(content: Data("ACK iOS\n".utf8), completion: .contentProcessed { sendError in
                Task { @MainActor in
                    self.status = sendError == nil ? "PASS: \(line.trimmingCharacters(in: .newlines))" : "FAIL: \(sendError!)"
                }
                connection.cancel()
            })
        }
    }
}

struct ProbeView: View {
    @StateObject private var probe = Probe()

    var body: some View {
        Text(probe.status)
            .padding()
            .onAppear { probe.start() }
    }
}

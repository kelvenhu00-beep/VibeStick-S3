import AVFoundation
import Foundation

final class VibeStickMicRecorder: NSObject, AVAudioRecorderDelegate {
    private let outputURL: URL
    private var recorder: AVAudioRecorder?
    private var didStop = false
    private var intSource: DispatchSourceSignal?
    private var termSource: DispatchSourceSignal?

    init(outputPath: String) {
        self.outputURL = URL(fileURLWithPath: outputPath)
        super.init()
    }

    func run() {
        AVCaptureDevice.requestAccess(for: .audio) { granted in
            DispatchQueue.main.async {
                guard granted else {
                    fputs("Microphone permission was denied\n", stderr)
                    exit(3)
                }
                self.startRecording()
            }
        }
        RunLoop.main.run()
    }

    private func startRecording() {
        do {
            try FileManager.default.createDirectory(
                at: outputURL.deletingLastPathComponent(),
                withIntermediateDirectories: true
            )
            let settings: [String: Any] = [
                AVFormatIDKey: Int(kAudioFormatMPEG4AAC),
                AVSampleRateKey: 16_000,
                AVNumberOfChannelsKey: 1,
                AVEncoderAudioQualityKey: AVAudioQuality.high.rawValue,
            ]
            let recorder = try AVAudioRecorder(url: outputURL, settings: settings)
            recorder.delegate = self
            recorder.isMeteringEnabled = false
            guard recorder.record() else {
                fputs("AVAudioRecorder failed to start\n", stderr)
                exit(4)
            }
            self.recorder = recorder
            print("recording \(outputURL.path)")
            fflush(stdout)
            installSignalHandlers()
        } catch {
            fputs("Recorder setup failed: \(error.localizedDescription)\n", stderr)
            exit(2)
        }
    }

    private func installSignalHandlers() {
        signal(SIGINT, SIG_IGN)
        signal(SIGTERM, SIG_IGN)

        intSource = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
        intSource?.setEventHandler { [weak self] in self?.stopAndExit() }
        intSource?.resume()

        termSource = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
        termSource?.setEventHandler { [weak self] in self?.stopAndExit() }
        termSource?.resume()
    }

    private func stopAndExit() {
        guard !didStop else { return }
        didStop = true
        recorder?.stop()
        print("stopped \(outputURL.path)")
        fflush(stdout)
        exit(0)
    }
}

guard CommandLine.arguments.count == 2 else {
    fputs("usage: vibe_stick_mic_recorder OUTPUT.m4a\n", stderr)
    exit(64)
}

VibeStickMicRecorder(outputPath: CommandLine.arguments[1]).run()

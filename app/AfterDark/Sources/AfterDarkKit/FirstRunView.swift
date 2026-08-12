import SwiftUI

// The first-run gate UI: consent, live download progress, missing-tool guidance,
// and error/retry. Shown in place of the main window until the asset tree is
// present. Cancelling quits the app (re-prompts next launch).
public struct FirstRunView: View {
    @ObservedObject var manager: FirstRunManager

    public init(manager: FirstRunManager) {
        self.manager = manager
    }

    public var body: some View {
        VStack(spacing: 20) {
            header
            content
        }
        .padding(40)
        .frame(minWidth: 520, maxWidth: 640)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color(nsColor: .windowBackgroundColor))
    }

    private var header: some View {
        VStack(spacing: 6) {
            Image(systemName: "moon.stars.fill")
                .font(.system(size: 40))
                .foregroundStyle(.yellow)
            Text("After Dark")
                .font(.largeTitle.weight(.semibold))
            Text("Screen saver modules")
                .font(.subheadline)
                .foregroundStyle(.secondary)
        }
    }

    @ViewBuilder
    private var content: some View {
        switch manager.stage {
        case .checking:
            ProgressView().controlSize(.small)
        case .needsConsent:
            consent
        case .needsTools(let msg):
            toolsMissing(msg)
        case .downloading:
            downloading
        case .failed(let msg):
            failed(msg)
        case .ready:
            // Parent swaps to the app when .ready; nothing to show.
            EmptyView()
        }
    }

    // MARK: - Consent
    private var consent: some View {
        VStack(spacing: 20) {
            Text("Download screen saver files")
                .font(.title2.weight(.medium))
            Text(FirstRunManager.disclaimer)
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.leading)
                .fixedSize(horizontal: false, vertical: true)
            HStack(spacing: 12) {
                Button("Quit") { NSApp.terminate(nil) }
                    .keyboardShortcut(.cancelAction)
                Button("Download") { manager.startDownload() }
                    .keyboardShortcut(.defaultAction)
                    .buttonStyle(.borderedProminent)
            }
            .padding(.top, 4)
        }
    }

    // MARK: - Missing tools
    private func toolsMissing(_ msg: String) -> some View {
        VStack(spacing: 18) {
            Image(systemName: "wrench.and.screwdriver")
                .font(.system(size: 28))
                .foregroundStyle(.orange)
            Text("Additional tools needed")
                .font(.title2.weight(.medium))
            Text(msg)
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.leading)
                .textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
            HStack(spacing: 12) {
                Button("Quit") { NSApp.terminate(nil) }
                Button("Re-check") { manager.refresh() }
                    .buttonStyle(.borderedProminent)
            }
        }
    }

    // MARK: - Downloading
    private var downloading: some View {
        VStack(spacing: 16) {
            Text(manager.phaseLabel.isEmpty ? "Working…" : manager.phaseLabel)
                .font(.title3.weight(.medium))
            ProgressView(value: manager.pct)
                .progressViewStyle(.linear)
                .frame(maxWidth: 380)
            HStack {
                Text("\(Int((manager.pct * 100).rounded()))%")
                    .monospacedDigit()
                if !manager.byteLabel.isEmpty {
                    Text("·").foregroundStyle(.secondary)
                    Text(manager.byteLabel).foregroundStyle(.secondary)
                }
            }
            .font(.caption)
            if !manager.lastLog.isEmpty {
                Text(manager.lastLog)
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .frame(maxWidth: 380)
            }
            Button("Cancel") {
                manager.cancel()
                NSApp.terminate(nil)
            }
            .padding(.top, 4)
        }
    }

    // MARK: - Failure
    private func failed(_ msg: String) -> some View {
        VStack(spacing: 18) {
            Image(systemName: "exclamationmark.triangle.fill")
                .font(.system(size: 28))
                .foregroundStyle(.red)
            Text("Download failed")
                .font(.title2.weight(.medium))
            ScrollView {
                Text(msg)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.leading)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(maxHeight: 160)
            HStack(spacing: 12) {
                Button("Quit") { NSApp.terminate(nil) }
                Button("Retry") { manager.startDownload() }
                    .buttonStyle(.borderedProminent)
            }
        }
    }
}

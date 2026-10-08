// SPDX-License-Identifier: GPL-2.0-or-later
// An isolated, user-driven Baidu login. Cookies never enter stdout or argv.
import Cocoa
import WebKit

final class LoginDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate, WKNavigationDelegate, WKUIDelegate {
    var window: NSWindow!
    var web: WKWebView!
    var status: NSTextField!
    var authorize: NSButton!
    let destination: URL
    var finished = false
    init(destination: URL) { self.destination = destination }

    func applicationDidFinishLaunching(_ notification: Notification) {
        let configuration = WKWebViewConfiguration()
        configuration.websiteDataStore = .nonPersistent()
        web = WKWebView(frame: .zero, configuration: configuration)
        web.navigationDelegate = self
        web.uiDelegate = self
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1080, height: 800),
                          styleMask: [.titled, .closable, .resizable, .miniaturizable],
                          backing: .buffered, defer: false)
        window.title = "shadPS4 · 百度网盘授权"
        window.delegate = self
        window.minSize = NSSize(width: 780, height: 600)
        let container = window.contentView!
        status = NSTextField(wrappingLabelWithString: "请在下方百度官方页面完成登录，然后点击右下角“授权给 shadPS4”。授权仅用于百度下载，凭据保存在本机私有目录。")
        status.font = .systemFont(ofSize: 14)
        authorize = NSButton(title: "授权给 shadPS4", target: self, action: #selector(saveAuthorization))
        authorize.bezelStyle = .rounded
        let cancel = NSButton(title: "取消", target: self, action: #selector(cancelLogin))
        cancel.bezelStyle = .rounded
        for view in [web!, status!, authorize!, cancel] {
            view.translatesAutoresizingMaskIntoConstraints = false
            container.addSubview(view)
        }
        NSLayoutConstraint.activate([
            status.topAnchor.constraint(equalTo: container.topAnchor, constant: 14),
            status.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: 18),
            status.trailingAnchor.constraint(equalTo: container.trailingAnchor, constant: -18),
            web.topAnchor.constraint(equalTo: status.bottomAnchor, constant: 12),
            web.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            web.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            web.bottomAnchor.constraint(equalTo: authorize.topAnchor, constant: -12),
            authorize.trailingAnchor.constraint(equalTo: container.trailingAnchor, constant: -18),
            authorize.bottomAnchor.constraint(equalTo: container.bottomAnchor, constant: -14),
            cancel.trailingAnchor.constraint(equalTo: authorize.leadingAnchor, constant: -12),
            cancel.centerYAnchor.constraint(equalTo: authorize.centerYAnchor)
        ])
        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        web.load(URLRequest(url: URL(string: "https://pan.baidu.com/disk/main")!))
    }
    func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction,
                 decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        guard let url = navigationAction.request.url else { decisionHandler(.cancel); return }
        if url.scheme == "about" { decisionHandler(.allow); return }
        let host = url.host?.lowercased() ?? ""
        decisionHandler(url.scheme == "https" && (host == "baidu.com" || host.hasSuffix(".baidu.com")) ? .allow : .cancel)
    }
    func webView(_ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration,
                 for navigationAction: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView? {
        if navigationAction.targetFrame == nil { webView.load(navigationAction.request) }
        return nil
    }
    @objc func saveAuthorization() {
        authorize.isEnabled = false
        web.configuration.websiteDataStore.httpCookieStore.getAllCookies { [weak self] cookies in
            guard let self = self else { return }
            let accepted = cookies.filter {
                let domain = $0.domain.lowercased().trimmingCharacters(in: CharacterSet(charactersIn: "."))
                return (domain == "baidu.com" || domain == "pan.baidu.com") &&
                    ($0.expiresDate == nil || $0.expiresDate! > Date())
            }
            let names = Set(accepted.map { $0.name })
            guard names.contains("BDUSS") && names.contains("STOKEN") else {
                self.status.stringValue = "尚未获得完整网盘登录状态。请先完成百度登录并进入网盘主页，再点击授权；如出现验证码，请在网页中自行完成。"
                self.authorize.isEnabled = true
                return
            }
            let cookie = accepted.map { "\($0.name)=\($0.value)" }.joined(separator: "; ")
            do {
                let parent = self.destination.deletingLastPathComponent()
                try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: true,
                                                        attributes: [.posixPermissions: 0o700])
                let data = try JSONSerialization.data(withJSONObject: ["schema": 1, "cookies": cookie])
                // Atomic replacement, with a private temporary file from the outset.
                let temporary = parent.appendingPathComponent(".account-\(UUID().uuidString).tmp")
                guard FileManager.default.createFile(atPath: temporary.path, contents: nil,
                                                     attributes: [.posixPermissions: 0o600]) else { throw CocoaError(.fileWriteUnknown) }
                let handle = try FileHandle(forWritingTo: temporary)
                try handle.write(contentsOf: data)
                try handle.synchronize()
                try handle.close()
                if rename(temporary.path, self.destination.path) != 0 {
                    try? FileManager.default.removeItem(at: temporary)
                    throw CocoaError(.fileWriteUnknown)
                }
                self.finished = true
                print("{\"schema\":1,\"event\":\"authorized\"}")
                NSApp.terminate(nil)
            } catch {
                self.status.stringValue = "无法保存本机授权文件，请检查数据目录权限后重试。"
                self.authorize.isEnabled = true
            }
        }
    }
    @objc func cancelLogin() { window.close() }
    func windowWillClose(_ notification: Notification) {
        if !finished { print("{\"schema\":1,\"event\":\"cancelled\"}") }
        NSApp.terminate(nil)
    }
}

if CommandLine.arguments.count != 2 || !CommandLine.arguments[1].hasPrefix("/") {
    exit(2)
}
let delegate = LoginDelegate(destination: URL(fileURLWithPath: CommandLine.arguments[1]))
let app = NSApplication.shared
app.setActivationPolicy(.regular)
app.delegate = delegate
app.run()

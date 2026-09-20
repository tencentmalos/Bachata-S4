"""Minimal stdio JSON-RPC client for the Litep MCP server (source checkout)."""
import json, os, subprocess, sys, threading, queue
ROOT = "C:/workspace/spatial_mcp_publish/dev_tools/mcp/litep"
ENV = dict(os.environ,
    LITEP_ANALYSIS_ENGINE=ROOT + "/analysis_engine/Ptracy.McpServer/bin/Debug/net8.0/Ptracy.McpServer.exe",
    LITEP_ALLOWED_ROOTS="C:\workspace", LITEP_ADB="C:/Users/Admin/AppData/Local/Android/Sdk/platform-tools/adb.exe",
    LITEP_DATA_ROOT="C:/workspace/emulations/shadps4/build/validation/litep-data", PYTHONIOENCODING="utf-8")
class Litep:
    def __init__(self):
        self.p = subprocess.Popen([sys.executable, ROOT + "/server.py"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=open("C:/workspace/emulations/shadps4/build/validation/litep-data/client-stderr.log", "ab"),
                                  text=True, encoding="utf-8", bufsize=1, env=ENV)
        self.q = queue.Queue(); self.n = 0
        threading.Thread(target=self._pump, daemon=True).start()
        self.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "cli", "version": "0"}})
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n"); self.p.stdin.flush()
    def _pump(self):
        for line in iter(self.p.stdout.readline, ""): self.q.put(line)
        self.q.put("")
    def rpc(self, method, params, timeout=900):
        self.n += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.n, "method": method, "params": params}) + "\n"); self.p.stdin.flush()
        while True:
            line = self.q.get(timeout=timeout)
            if not line: raise RuntimeError("server closed")
            msg = json.loads(line)
            if msg.get("id") == self.n:
                if "error" in msg: raise RuntimeError(msg["error"])
                return msg["result"]
    def call(self, name, **args):
        r = self.rpc("tools/call", {"name": name, "arguments": args})
        if r.get("isError"): raise RuntimeError(r.get("structuredContent") or r["content"])
        return r.get("structuredContent") or json.loads(r["content"][0]["text"])
    def tools(self):
        return [t["name"] for t in self.rpc("tools/list", {})["tools"]]
if __name__ == "__main__":
    l = Litep(); print(len(l.tools()), "tools"); print(sorted(l.tools()))

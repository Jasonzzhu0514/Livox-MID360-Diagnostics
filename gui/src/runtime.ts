export function isTauriRuntime() {
  return typeof window.__TAURI_IPC__ === "function";
}

export function tauriRequiredMessage() {
  return "This action requires the Tauri desktop window. Browser preview at http://127.0.0.1:1420 only renders the UI.";
}

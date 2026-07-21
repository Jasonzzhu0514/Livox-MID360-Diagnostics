import { open as openExternal } from "@tauri-apps/api/shell";
import { ExternalLink, Github, Radar, X } from "lucide-react";
import { MouseEvent, useEffect, useRef, useState } from "react";

const PROJECT_URL = "https://github.com/Jasonzzhu0514/Livox-MID360-Diagnostics";

type Props = {
  version: string;
  onClose: () => void;
};

function isTauriRuntime() {
  return typeof window.__TAURI_IPC__ === "function";
}

export function AboutDialog({ version, onClose }: Props) {
  const dialogRef = useRef<HTMLElement | null>(null);
  const closeButtonRef = useRef<HTMLButtonElement | null>(null);
  const [linkError, setLinkError] = useState("");

  useEffect(() => {
    const previousFocus = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    closeButtonRef.current?.focus();

    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape") {
        onClose();
        return;
      }
      if (event.key !== "Tab") {
        return;
      }

      const focusable = Array.from(
        dialogRef.current?.querySelectorAll<HTMLElement>("button:not([disabled]), a[href]") ?? [],
      );
      if (focusable.length === 0) {
        event.preventDefault();
        return;
      }
      const first = focusable[0];
      const last = focusable[focusable.length - 1];
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first.focus();
      }
    };
    window.addEventListener("keydown", onKeyDown);

    return () => {
      window.removeEventListener("keydown", onKeyDown);
      previousFocus?.focus();
    };
  }, [onClose]);

  async function handleProjectLink(event: MouseEvent<HTMLAnchorElement>) {
    if (!isTauriRuntime()) {
      return;
    }

    event.preventDefault();
    setLinkError("");
    try {
      await openExternal(PROJECT_URL);
    } catch (error) {
      setLinkError(`Could not open the repository: ${String(error)}`);
    }
  }

  return (
    <div className="about-backdrop" onMouseDown={(event) => event.target === event.currentTarget && onClose()}>
      <section ref={dialogRef} className="about-dialog" role="dialog" aria-modal="true" aria-labelledby="about-title">
        <header className="about-header">
          <div className="about-mark" aria-hidden="true">
            <Radar size={29} strokeWidth={2.2} />
          </div>
          <div className="about-heading">
            <span>About</span>
            <h2 id="about-title">Livox MID360 Diagnostics</h2>
          </div>
          <button ref={closeButtonRef} type="button" className="about-close" onClick={onClose} aria-label="Close About" title="Close">
            <X size={18} />
          </button>
        </header>

        <div className="about-body">
          <p className="about-summary">
            An open-source Linux desktop utility for discovering and diagnosing Livox MID360 and MID360S lidar,
            monitoring device health and data streams, and previewing live point clouds.
          </p>

          <dl className="about-facts">
            <div>
              <dt>Version</dt>
              <dd>{version}</dd>
            </div>
            <div>
              <dt>Author</dt>
              <dd>jasonzzhu</dd>
            </div>
            <div>
              <dt>License</dt>
              <dd>MIT</dd>
            </div>
            <div>
              <dt>Platform</dt>
              <dd>Ubuntu / Linux</dd>
            </div>
          </dl>

          <div className="about-repository">
            <div className="about-repository-label">
              <Github size={17} aria-hidden="true" />
              <span>Open-source repository</span>
            </div>
            <a href={PROJECT_URL} target="_blank" rel="noreferrer" onClick={handleProjectLink}>
              <span>{PROJECT_URL}</span>
              <ExternalLink size={15} aria-hidden="true" />
            </a>
            {linkError && <p className="about-link-error">{linkError}</p>}
          </div>
        </div>

        <footer className="about-footer">
          <span>Copyright 2026 jasonzzhu</span>
          <span>Built for Livox MID360 diagnostics</span>
        </footer>
      </section>
    </div>
  );
}

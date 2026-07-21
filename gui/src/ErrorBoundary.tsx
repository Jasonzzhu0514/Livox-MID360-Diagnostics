import { Component, type ErrorInfo, type ReactNode } from "react";

type Props = {
  children: ReactNode;
  fallbackTitle: string;
};

type State = {
  error: string | null;
};

export class ErrorBoundary extends Component<Props, State> {
  state: State = {
    error: null,
  };

  static getDerivedStateFromError(error: unknown): State {
    return {
      error: error instanceof Error ? error.message : String(error),
    };
  }

  componentDidCatch(error: unknown, info: ErrorInfo) {
    console.error(this.props.fallbackTitle, error, info);
  }

  render() {
    if (this.state.error) {
      return (
        <section className="module-error">
          <h1>{this.props.fallbackTitle}</h1>
          <p>{this.state.error}</p>
        </section>
      );
    }
    return this.props.children;
  }
}

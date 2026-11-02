/**
 * Transport-level errors. Part of the api-client layer: no app semantics here,
 * just "the request failed and here is what the server said".
 */

export class ApiError extends Error {
  readonly status: number;
  readonly detail: string | null;

  constructor(message: string, status: number, detail: string | null = null) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.detail = detail;
  }

  get isClientError(): boolean {
    return this.status >= 400 && this.status < 500;
  }

  get isServerError(): boolean {
    return this.status >= 500;
  }
}

export class NetworkError extends Error {
  override readonly cause: unknown;

  constructor(message: string, cause?: unknown) {
    super(message);
    this.name = 'NetworkError';
    this.cause = cause;
  }
}

export function isApiError(e: unknown): e is ApiError {
  return e instanceof ApiError;
}

export function isNetworkError(e: unknown): e is NetworkError {
  return e instanceof NetworkError;
}

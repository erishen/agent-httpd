// Shared client/server types. Kept out of App.tsx so page modules and the
// render core can import them without pulling the whole component tree.

export interface Params {
  [key: string]: string;
}

// SSR renders the App on the server (markup inside #root, then hydrateRoot);
// CSR ships an empty shell and renders entirely with createRoot.
export type RenderMode = "ssr" | "csr";

export function isRenderMode(value: string | undefined): value is RenderMode {
  return value === "ssr" || value === "csr";
}
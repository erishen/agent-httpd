// Tailwind output is imported as a plain string by render.tsx and inlined
// into the SSR <style> block via esbuild's --loader:.css?raw=text. This gives
// tsc (--noEmit) the module shape it needs without Node CSS imports. The
// ?raw suffix is what makes Vite's dev SSR pipeline return the text too.
declare module "*.css" {
  const content: string;
  export default content;
}

declare module "*.css?raw" {
  const content: string;
  export default content;
}
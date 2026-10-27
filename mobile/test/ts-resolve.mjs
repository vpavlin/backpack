// node --import ./test/ts-resolve.mjs --experimental-strip-types test/x.test.ts
// The app's sources import each other without extensions (Metro/tsc style); Node needs ".ts". This
// hook retries an unresolved relative import with ".ts" appended. Tests only.
import { register } from "node:module";
register("data:text/javascript," + encodeURIComponent(`
export async function resolve(spec, ctx, next) {
  try { return await next(spec, ctx); }
  catch (e) { if (spec.startsWith(".") && !/\\.[a-z]+$/.test(spec)) return next(spec + ".ts", ctx); throw e; }
}`));

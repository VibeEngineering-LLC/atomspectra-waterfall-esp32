// CSRF-1: post() must refetch the CSRF token and retry once on 403. Usage: node test/web/csrf_retry_test.mjs [git-ref]
import { readFileSync } from "node:fs";
import { execSync } from "node:child_process";
const ref = process.argv[2];
const pages = ["web/index.html", "web/saved.html", "web/service.html", "web/system.html", "web/waterfall.html"];
let failed = 0;
for (const path of pages) {
    const content = ref ? execSync(`git show ${ref}:${path}`).toString() : readFileSync(path, "utf8");
    const idx = content.indexOf("function post(url,");
    if (idx === -1) throw new Error(`Not found in ${path}`);
    let start = idx;
    if (content.substring(idx - 6, idx) === "async ") start -= 6;
    let depth = 0, end = -1;
    for (let i = content.indexOf("{", start); i < content.length; i++) {
        if (content[i] === "{") depth++;
        else if (content[i] === "}") { depth--; if (depth === 0) { end = i + 1; break; } }
    }
    if (end === -1) throw new Error(`No closing brace in ${path}`);
    const src = content.substring(start, end);
    let board = "NEW", calls = 0;
    const ctx = { csrfToken: "STALE" };
    async function fetch(url, o) {
        calls++;
        if (url === "/api/csrf-token") return { json: async () => ({ token: board }) };
        return { status: o.headers["X-CSRF-Token"] === board ? 200 : 403 };
    }
    async function loadCsrf() { const r = await (await fetch("/api/csrf-token")).json(); ctx.csrfToken = r.token; }
    const post = new Function("fetch", "loadCsrf", "ctx", `with(ctx){ ${src}; return post; }`)(fetch, loadCsrf, ctx);
    const arg = path.includes("waterfall") ? { a: 1 } : { body: "-sto" };
    try {
        const r1 = await post("/api/command", arg);
        calls = 0;
        const r2 = await post("/api/command", arg);
        if (r1.status === 200 && r2.status === 200 && calls === 1) console.log(`OK   ${path} stale->${r1.status} fresh->${r2.status} calls=${calls}`);
        else { failed++; console.log(`FAIL ${path} stale->${r1.status} fresh->${r2.status} calls=${calls}`); }
    } catch (e) { failed++; console.log(`FAIL ${path} Error: ${e.message}`); }
}
console.log(`failed: ${failed}`);
process.exit(failed ? 1 : 0);

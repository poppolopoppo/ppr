// Compare two stage-2 output directories byte-for-byte, ignoring files that are
// environment dependent by design (timings.json carries wall-clock durations).
//
//   node tools/compare-dirs.mjs out/stage2 out/stage2-repeat

"use strict";

import {readdirSync, readFileSync, statSync} from "node:fs";
import {createHash} from "node:crypto";
import {join} from "node:path";

const [dirA, dirB] = process.argv.slice(2);
if(!dirA || !dirB)
{
    console.error("usage: node tools/compare-dirs.mjs <dirA> <dirB>");
    process.exit(2);
}

const IGNORED = new Set(["timings.json"]);

const filesA = readdirSync(dirA).filter((name) => !IGNORED.has(name)).sort();
const filesB = readdirSync(dirB).filter((name) => !IGNORED.has(name)).sort();

if(filesA.join("\n") !== filesB.join("\n"))
{
    console.error(`file lists differ:\n  ${dirA}: ${filesA.join(", ")}\n  ${dirB}: ${filesB.join(", ")}`);
    process.exit(1);
}

let failures = 0;

for(const name of filesA)
{
    if(!statSync(join(dirA, name)).isFile())
    {
        continue;
    }

    const bytesA = readFileSync(join(dirA, name));
    const bytesB = readFileSync(join(dirB, name));
    const shaA = createHash("sha256").update(bytesA).digest("hex");
    const shaB = createHash("sha256").update(bytesB).digest("hex");

    if(shaA === shaB)
    {
        console.log(`${shaA}  ${name}`);
    }
    else
    {
        failures += 1;
        console.error(`MISMATCH  ${name}  ${shaA} != ${shaB}`);
    }
}

if(failures !== 0)
{
    console.error(`${failures} file(s) differed`);
    process.exit(1);
}

console.log(`OK: ${filesA.length} files byte-identical (ignoring ${[...IGNORED].join(", ")})`);

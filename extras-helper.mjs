#!/bin/env node
/**
The MIT License (MIT)

Copyright (c) 2024-present karurochari <public@karurochari.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

const protocol_version = [2, 1, 0]
const runtime_version = [0, 4, 2]   //TODO: take this from somewhere else.

import { readFile, writeFile, mkdir, rm, cp } from 'node:fs/promises'
import { dirname } from 'node:path'
import util from 'node:util';

import { program } from 'commander';
import { exec as _exec } from 'node:child_process';
const exec = util.promisify(_exec);

import { Readable } from 'node:stream'
import fg from 'fast-glob'

/**
 * It checks the version of the runtime & module interface against the one presented by a module
 * @param {string} name 
 * @param {[number,number,number]|undefined} modprotocol_version 
 * @param {[number,number,number]|undefined} runtime_version 
 * @param {boolean} strict 
 * @returns {boolean} True if versions are matching
 */
function check_versions(strict, name, modprotocol_version, runtime_version) {
    if (strict) {
        if (modprotocol_version === undefined) {
            console.error(
                `Module <tjs:${name}> is not versioned against the module protocol.`,
            );
            process.exit(1);
        }
        if (runtime_version === undefined) {
            console.error(
                `Module <tjs:${name}> is not versioned against the runtime.`,
            );
            process.exit(1);
        }
    }
    else {
        if (modprotocol_version === undefined)
            console.warning(
                `Module <tjs:${name}> has no protocol version reported. It might end up being incompatible`,
            );
        if (runtime_version === undefined)
            console.warning(
                `Module <tjs:${name}> has no runtime version reported. It might end up being incompatible`,
            );
    }

    //TODO: Logic to determine if they are compatible here.
    return true;
}

/**
 * 
 * @param {string} name Name of the submodule
 * @param {string} url Source
 * @param {string} branch Branch/Tag
 */
async function clone_shallow(name, url, branch) { }

async function copy_template(path, subdir) {
    const files = await fg(`./extras/${path}/${subdir}/*.js`);
    const prefix = `./${subdir}/${path}/${subdir}/`.length
    const suffix = ".js".length
    for (const file of files) {
        const name = file.substring(prefix, file.length - suffix).replaceAll("[module]", path)
        await writeFile(`./${subdir}/extras/${name}.js`, ((await readFile(file)).toString().replaceAll('__MODULE__', path)))

    }
}

async function retrieve(name, path) {
    //From the internet
    if (path.startsWith('https://') || path.startsWith('http://')) {
        await writeFile(
            `./extras/${name}.tar.gz`,
            Readable.fromWeb(
                (await fetch(path)).body,
            ),
        )
        await exec(`mkdir ./extras/${name} &&  tar -xvzf ./extras/${name}.tar.gz -C ./extras/${name} --strip-components=1`);
        await rm(`./extras/${name}.tar.gz`)
    }
    //Local folder
    else {
        await cp(path, `./extras/${name}`, { recursive: true, dereference: true, errorOnExist: false })
    }
    await install(name)
}

async function install(path) {
    await mkdir(`src/extras/${path}`, { errorOnExist: false });

    //Copy over all files in src
    {
        const files = await fg(`./extras/${path}/src/**/*`);
        const prefix = `./extras/${path}/src/`.length
        for (const file of files) {
            const name = file.substring(prefix).replaceAll("[module]", path)
            const fullPath = `./src/extras/${path}/${name}`
            await mkdir(dirname(fullPath), { errorOnExist: false, recursive: true });
            await writeFile(fullPath, ((await readFile(file)).toString().replaceAll('__MODULE__', path)))

        }
    }

    //While js/ts files must be already reduced in a bundle by this point.
    await writeFile(`./src/js/extras/${path}.js`, ((await readFile(`./extras/${path}/bundle/[module].js`)).toString().replaceAll('__MODULE__', path)))
    await writeFile(`./docs/types/extras/${path}.d.ts`, ((await readFile(`./extras/${path}/bundle/[module].d.ts`)).toString().replaceAll('__MODULE__', path)))
    await copy_template(path, 'examples')
    await copy_template(path, 'benchmarks')
    await copy_template(path, 'tests')
}

async function clear() {
    await rm('extras/', { recursive: true, force: true });
    await rm('src/extras/', { recursive: true, force: true });
    await rm('src/js/extras/', { recursive: true, force: true });
    await rm('tests/extras/', { recursive: true, force: true });
    await rm('examples/extras/', { recursive: true, force: true });
    await rm('deps/extras/', { recursive: true, force: true });
    await rm('benchmark/extras/', { recursive: true, force: true });
    await rm('docs/types/extras/', { recursive: true, force: true });

    await rm('./src/extras-bootstrap.c.frag', { force: true })
    await rm('./src/extras-headers.c.frag', { force: true })
    await rm('./src/extras-bundles.c.frag', { force: true })
    await rm('./src/extras-entries.c.frag', { force: true })
}

program
    .name('extras-helper.mjs')
    .description('A CLI to customize your txiki distribution');

program.command('clear')
    .description('Clear after your previous configuration')
    .action(async () => {
        await clear()
    })

program.command('refresh')
    .description('Refresh a single module, keeping the rest the same')
    .argument("<module>", 'module name ')
    .argument("[filename]", 'filename for the configuration', './modules.json')
    .action(async (modname, filename) => {
        let config = undefined
        try {
            config = JSON.parse(await readFile(filename))
        }
        catch (e) {
            console.error("Unable to parse the config file.")
            process.exit(1)
        }

        await retrieve(modname, config[modname])
    })

program.command('clone')
    .description('Clear after your previous configuration')
    .argument("[filename]", 'filename for the configuration', './modules.json')
    .action(async (filename) => {
        //For now, since I am too lazy to handle merging
        await clear()

        await mkdir("extras/", { errorOnExist: false });
        await mkdir('src/extras/', { errorOnExist: false });
        await mkdir('src/js/extras/', { errorOnExist: false });
        await mkdir('tests/extras/', { errorOnExist: false });
        await mkdir('examples/extras/', { errorOnExist: false });
        await mkdir('deps/extras/', { errorOnExist: false });
        await mkdir('benchmark/extras/', { errorOnExist: false });
        await mkdir('docs/types/extras/', { errorOnExist: false });

        let config = undefined
        try {
            config = JSON.parse(await readFile(filename))
        }
        catch (e) {
            console.error("Unable to parse the config file.")
            process.exit(1)
        }

        for (const module of Object.entries(config)) {
            await retrieve(module[0], module[1])
        }

        //Placeholder for now
        await writeFile('deps/extras/CMakeLists.txt', '')
        await writeFile('./modules.json', JSON.stringify(config, null, 4))

        //Construct src/extras.bootstrap to initialize the extra modules
        await writeFile('./src/extras-bootstrap.c.frag', Object.keys(config).map(x => `tjs__mod_${x}_init(ctx, ns);`).join('\n'))
        await writeFile('./src/extras-headers.c.frag', Object.keys(config).map(x => `#include "./extras/${x}/module.h"`).join('\n'))
        await writeFile('./src/extras-bundles.c.frag', Object.keys(config).map(x => `#include "bundles/c/extras/${x}.c"`).join('\n'))
        await writeFile('./src/extras-entries.c.frag', Object.keys(config).map(x => `{ "tjs:${x}", tjs__${x}, tjs__${x}_size},`).join('\n'))

        //Construct the ts header
        await writeFile('./docs/types/extras/index.d.ts', Object.keys(config).map(x => `import "./${x}.d.ts";`).join('\n'))
    })


program.parse();

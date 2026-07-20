import commonjs from "@rollup/plugin-commonjs";
import nodeResolve from "@rollup/plugin-node-resolve";
import terser from "@rollup/plugin-terser";
import typescript from "@rollup/plugin-typescript";

const pluginDirectory = "com.n0ahtm.audioplaybackconnector2.sdPlugin";

export default {
  input: "src/plugin.ts",
  output: {
    file: `${pluginDirectory}/bin/plugin.js`,
    format: "es"
  },
  plugins: [
    typescript(),
    nodeResolve({
      browser: false,
      exportConditions: ["node"],
      preferBuiltins: true
    }),
    commonjs(),
    terser(),
    {
      name: "emit-module-package-file",
      generateBundle() {
        this.emitFile({
          fileName: "package.json",
          source: '{ "type": "module" }',
          type: "asset"
        });
      }
    }
  ]
};

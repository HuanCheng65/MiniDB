#!/usr/bin/env python3
"""Draws a report-sized picture of a two-level B+ tree from a
`PRAGMA index_tree` dump: the root, the first two and the last two leaves.

    scripts/index_figure.py supplier_key.tree supplier_key.png

Needs Graphviz (`dot`). The full tree is in the .tree/.dot files; this only
shortens it to fit a page, and says how many leaves it leaves out.
"""
import re
import subprocess
import sys

NODE = re.compile(r'(INTERNAL|LEAF) p(\d+) \[(\d+)\] (.*)')
KEY = re.compile(r'"((?:[^"\\]|\\.)*)"@\d+:\d+')


def main(tree_path, png_path):
    lines = open(tree_path, encoding='utf-8').read().splitlines()
    title = re.match(r'B\+ tree (\S+) \((\d+) entries\)', lines[0])
    nodes = [NODE.search(line).groups() for line in lines[1:] if NODE.search(line)]
    root, leaves = nodes[0], nodes[1:]
    if root[0] != 'INTERNAL' or any(kind != 'LEAF' for kind, *_ in leaves):
        sys.exit('expected a complete two-level tree (root plus leaves)')

    def keys(text, count):
        return [k.replace('\\', '\\\\') for k in KEY.findall(text)[:count]]

    shown = leaves[:2] + leaves[-2:] if len(leaves) > 4 else leaves
    out = [
        'digraph {',
        '  graph [fontname="Helvetica", labelloc=t, labeljust=l, fontsize=20,'
        ' bgcolor="#f7f9fc", nodesep=0.5, ranksep=0.9, pad=0.4,',
        f'    label="B+ tree: {title[1]}\\l{int(title[2]):,} entries | 8 KB pages'
        f' | height 2 | {len(leaves)} leaf pages\\l "];',
        '  node [fontname="Helvetica", shape=box, style="rounded,filled",'
        ' fillcolor=white, color="#7a9cc0", margin="0.25,0.15"];',
        '  edge [color="#62788f"];',
        f'  root [fillcolor="#173d66", fontcolor=white, label="Internal p{root[1]}'
        f'  |  {root[2]} separator keys\\l{" | ".join(keys(root[3], 2))} | ...\\l"];',
    ]
    for kind, page, count, text in shown:
        following = re.search(r'-> (\S+)$', text)[1]
        first = '\\l'.join(keys(text, 2))  # no backslash in f-string braces
        out.append(f'  p{page} [label="Leaf p{page} | {count} entries\\l'
                   f'{first}\\l...\\lnext: {following}\\l"];')
        out.append(f'  root -> p{page};')
    chain = [f'p{page}' for _, page, _, _ in shown]
    if len(leaves) > 4:
        out.append(f'  gap [shape=plaintext, style="", fontcolor="#3b6ea5",'
                   f' label="...\\n{len(leaves) - 4} leaves\\nomitted"];')
        chain.insert(2, 'gap')
        out.append('  root -> gap [style=invis];')
    out.append('  { rank=same; ' + ' -> '.join(chain) +
               ' [style=dashed, color="#2f6fb5"]; }')
    out.append('}')
    subprocess.run(['dot', '-Tpng', '-Gdpi=110', '-o', png_path],
                   input='\n'.join(out).encode(), check=True)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])

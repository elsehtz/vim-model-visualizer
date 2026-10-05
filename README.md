# stlview

A rotating STL viewer for the terminal. Renders a solid, z-buffered,
Lambert-shaded model using Unicode upper-half-block glyphs (`▀`) with truecolor —
two vertical pixels per character cell — plus a braille (`⠿`) wireframe mode for
crisp line-art at 2×4 sub-cell resolution.

Single C file, libc + libm only. Linux / POSIX terminals with truecolor
(most modern ones: foot, kitty, alacritty, wezterm, gnome-terminal, recent xterm).

## Build

```sh
make                # -> ./stlview
make testmodels     # generates cube.stl and torus.stl to play with
make install        # copies to ~/.local/bin (ensure it's on $PATH)
```

## Run

```sh
./stlview torus.stl
./stlview --wireframe cube.stl
./stlview --perspective torus.stl
./stlview --fps 60 model.stl
```

### Controls

| key            | action                                        |
|----------------|-----------------------------------------------|
| `q` / `Ctrl-C` | quit                                          |
| `space`        | pause / resume auto-rotation                  |
| arrow keys     | nudge rotation                                |
| `+` / `-`      | zoom in / out                                 |
| `w`            | toggle solid / wireframe                      |
| `p`            | toggle orthographic / perspective projection  |
| `[` / `]`      | shorten / lengthen focal length (persp. only) |
| `r`            | reset view                                     |

## Using it from vim

`stlview` is a standalone CLI, so the simplest integration is a mapping that
runs it on the file in the current buffer. Add to your `~/.vimrc`:

```vim
" View the STL in the current buffer (q to return to vim)
nnoremap <leader>v :execute '!stlview ' . shellescape(expand('%'))<CR>
```

Or open it in a split terminal so vim stays visible (Vim 8+/Neovim):

```vim
nnoremap <leader>v :execute 'terminal stlview ' . shellescape(expand('%'))<CR>
```

For browsing: place the cursor over an `.stl` filename (e.g. in netrw) and use
`gf`-style flow, or just `:!stlview %` when the STL itself is the open buffer.

## How it works

1. **Load** — auto-detects binary STL (exact `84 + 50·n` byte size) vs ASCII
   (scans for `vertex` triples). Triangle normals are recomputed from geometry,
   so unreliable stored normals don't matter.
2. **Normalize** — center on the bounding-box midpoint and scale to a unit
   bounding *sphere*, so the model never clips while rotating.
3. **Rotate** — each frame applies a Y-then-X rotation; auto-spin is
   time-based, so speed is independent of frame rate.
4. **Rasterize** — orthographic *or* perspective projection (the `x/z, y/z`
   depth divide; press `p`), half-resolution z-buffer, barycentric triangle fill.
   Lighting is two-sided Lambert (winding-agnostic) so arbitrary STLs shade
   correctly without backface guesswork.
5. **Output** — half-block cells map two pixel rows per character; color escapes
   are emitted only when they change (run-length), and the whole frame is written
   in a single `write()` to avoid flicker. Uses the alternate screen buffer and
   restores the terminal cleanly on exit or resize (`SIGWINCH`).

## Ideas to extend

- **Depth-tinted wireframe** — color braille cells by average depth for a sense
  of volume without full shading.
- **Material/colormap flags** — `--color`, or a normal-direction colormap.
- **Mouse drag to orbit** — enable SGR mouse reporting and map drags to ay/ax.
- **ASCII ramp mode** — a `·:+*#█` fallback for terminals without truecolor.

# Debug HUD attribution

The texture-free packed-ASCII draw layout and bitmap lookup approach are
adapted from the technique described in [Rendering text without textures]
(https://habr.com/ru/companies/beget/articles/859796/) and the primary
`tgfrerer/island` implementation at commit
[`3ee89fb531f3e2b5571ceced8823d31637af8714`](https://github.com/tgfrerer/island/blob/3ee89fb531f3e2b5571ceced8823d31637af8714/modules/le_debug_print_text/shaders/debug_text.frag),
in particular the `le_debug_print_text` module. The upstream project is MIT
licensed:

Copyright (c) 2020 Tim Gfrerer

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

The Tamsyn terms are kept separately in `TAMSYN_LICENSE.txt`. They were
checked independently against the font's published license copy at
https://github.com/sunaku/tamzen-font/blob/master/LICENSE and are not covered
by the Island MIT notice above.

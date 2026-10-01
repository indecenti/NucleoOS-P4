# textwrap for NucleoOS: the common API of CPython's textwrap (wrap, fill, shorten, dedent,
# indent) without its regular expressions, which MicroPython's re cannot compile.


def wrap(text, width=70, initial_indent="", subsequent_indent="", break_long_words=True):
    lines, line, prefix = [], "", initial_indent
    for word in text.split():
        while break_long_words and len(prefix) + len(word) > width and not line:
            cut = max(1, width - len(prefix))
            lines.append(prefix + word[:cut])
            word, prefix = word[cut:], subsequent_indent
            if not word:
                break
        if not word:
            continue
        if line and len(prefix) + len(line) + 1 + len(word) > width:
            lines.append(prefix + line)
            line, prefix = word, subsequent_indent
        else:
            line = line + " " + word if line else word
    if line:
        lines.append(prefix + line)
    return lines


def fill(text, width=70, **kw):
    return "\n".join(wrap(text, width, **kw))


def shorten(text, width, placeholder=" [...]"):
    text = " ".join(text.split())
    if len(text) <= width:
        return text
    out = ""
    for word in text.split():
        if len(out) + len(word) + 1 + len(placeholder) > width:
            break
        out = out + " " + word if out else word
    return out + placeholder


def dedent(text):
    lines = text.split("\n")
    margin = None
    for l in lines:
        s = l.lstrip()
        if s:
            n = len(l) - len(s)
            margin = n if margin is None else min(margin, n)
    if not margin:
        return text
    return "\n".join(l[margin:] if l.strip() else l.strip(" \t") for l in lines)


def indent(text, prefix, predicate=None):
    out = []
    for l in text.split("\n"):
        keep = predicate(l) if predicate else l.strip()
        out.append(prefix + l if keep else l)
    return "\n".join(out)

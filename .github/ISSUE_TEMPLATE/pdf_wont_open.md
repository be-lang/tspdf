---
name: A PDF won't open
about: tspdf fails on a PDF that other tools can read
title: 'Fails to open: '
labels: compatibility
assignees: ''
---

Broad real-world compatibility is a primary goal, so these reports are very
valuable.

**Command and error**
The command you ran and the exact error, e.g.:
```
$ tspdf info mydoc.pdf
tspdf info: failed to open 'mydoc.pdf': cross-reference table error
```

**Can another tool open it?**
If you know, note whether `qpdf --check`, `mutool`, or a viewer opens the file,
and what they report.

**The file**
Please attach the PDF if you can share it, or a minimal version that still
reproduces the failure. If it's sensitive, see [SECURITY.md](../../SECURITY.md)
for a private channel, or describe how it was produced (which tool/version
created it, whether it is encrypted, linearized, etc.).

**Environment**
- tspdf version: `tspdf --version`
- OS and compiler

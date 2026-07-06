# DPOR Notes

Start with source-DPOR or persistent-set style reduction only after naive exploration is trusted.

The independence predicate must consider:

- memory address conflicts
- synchronization object conflicts
- enabledness changes
- thread creation/join once modeled
- I/O or external effects once modeled

"""Allow `python -m quasar`."""

from quasar.quasar import main

if __name__ == "__main__":
    raise SystemExit(main())

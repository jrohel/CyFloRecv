\[0.1.0] - 2024-05-16
---------------------
- First version of CyFlowRec

    Usage: `cyflowrec --storage-dir=<path> --port-dev=<port> [--help]`


\[0.2.0] - 2024-05-18
---------------------
- Added command line argument `--storage-create-dirs=<0/1>`

    Disables/enables the creation of missing directories in the storage path.

    - `1` - directory creation enabled
    - `0` - directory creation disabled


\[0.2.1] - 2024-05-19
---------------------
- Added more information to `--help`


\[0.2.2] - 2024-05-20
---------------------
- Discard or truncate only files that failed to save

    In previous versions, if a received file could not be saved, it was
    discarded along with all subsequent files in the current transfer.
    The end of the transfer is detected by the expiration of a timeout
    without data (intercharacter).

    In this version, only the file that failed to save is discarded/truncated.
    The following files are not affected.

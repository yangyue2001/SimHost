import sys


def clock_samples(path):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    clock_id = None
    for line in lines:
        line = line.strip()
        if not line.startswith("$var"):
            continue
        parts = line.split()
        if len(parts) >= 5 and parts[4] == "clock":
            clock_id = parts[3]
            break
    if clock_id is None:
        raise SystemExit("clock id not found in %s" % path)

    samples = []
    seen = False
    value = None
    for raw in lines:
        line = raw.strip()
        if line.startswith("#"):
            if seen and value is not None:
                samples.append(value)
            seen = True
            continue
        if not line or line.startswith("$"):
            continue
        token_value = None
        token_id = None
        if line[0] in "01xXzZ" and not line.startswith("b"):
            token_value = line[0]
            token_id = line[1:]
        elif line.startswith("b"):
            parts = line.split()
            if len(parts) == 2:
                token_value = parts[0][1:]
                token_id = parts[1]
        if token_id == clock_id:
            value = token_value
    if seen and value is not None:
        samples.append(value)
    return samples


def main():
    path = sys.argv[1]
    mode = sys.argv[2]
    samples = clock_samples(path)
    print("samples", len(samples), "head", "".join(samples[:16]))
    if mode == "pos":
        if not samples or any(item != "1" for item in samples):
            raise SystemExit("posedge-only waveform contains a non-1 clock")
    elif mode == "full":
        if len(samples) < 2 or any(item not in ("0", "1") for item in samples):
            raise SystemExit("full waveform missing both clock levels")
        if any(left == right for left, right in zip(samples, samples[1:])):
            raise SystemExit("full waveform clock did not alternate")
    elif mode == "empty":
        if samples:
            raise SystemExit("log window still recorded clock samples")
    else:
        raise SystemExit("unknown mode %s" % mode)


if __name__ == "__main__":
    main()

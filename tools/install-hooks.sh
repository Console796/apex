#!/bin/sh
# Opt-in: install a pre-push hook that runs the fast CI subset.
set -e
root=$(git rev-parse --show-toplevel)
cat > "$root/.git/hooks/pre-push" <<'HOOK'
#!/bin/sh
exec python3 "$(git rev-parse --show-toplevel)/tools/ci.py" --fast
HOOK
chmod +x "$root/.git/hooks/pre-push"
echo "installed $root/.git/hooks/pre-push"

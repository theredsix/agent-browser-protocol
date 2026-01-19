#!/bin/bash
# Run ABP navigation test with the correct virtualenv

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Initialize pyenv
export PYENV_ROOT="$HOME/.pyenv"
export PATH="$PYENV_ROOT/bin:$PATH"
eval "$(pyenv init -)"
eval "$(pyenv virtualenv-init -)"

# Activate the abp virtualenv
pyenv activate abp

# Run the test
python "$SCRIPT_DIR/test_navigation.py" "$@"

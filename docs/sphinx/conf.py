"""Sphinx configuration for IPC Client-Server documentation."""
import os

project = "IPC Client-Server System"
author = "Developer"
copyright = "2026"

extensions = [
    "breathe",
]

# Breathe: point at Doxygen XML output
breathe_projects = {
    "ipc": os.path.join(
        os.path.dirname(__file__), "..", "..", "build", "docs", "doxygen", "xml"
    ),
}
breathe_default_project = "ipc"

# General
templates_path = []
exclude_patterns = ["_build"]
html_theme = "alabaster"

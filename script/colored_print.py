import sys

class Color:
    """Simple ANSI color codes, works on most terminals."""
    RESET = "\033[0m"
    BOLD = "\033[1m"
    RED = "\033[91m"
    GREEN = "\033[92m"
    YELLOW = "\033[93m"
    CYAN = "\033[96m"
    GRAY = "\033[90m"

    @staticmethod
    def colorize(text, color, bold=False):
        if not sys.stdout.isatty():
            return text
        bold_code = Color.BOLD if bold else ""
        return f"{bold_code}{color}{text}{Color.RESET}"

def print_pass(text="PASS"):
    print(Color.colorize(text, Color.GREEN, bold=True))

def print_fail(msg, prefix="FAIL"):
    print(Color.colorize(f"{prefix} - {msg}", Color.RED, bold=True))

def print_info(msg):
    print(Color.colorize(msg, Color.CYAN))

def print_warning(msg):
    print(Color.colorize(msg, Color.YELLOW))

def print_section(title):
    print(Color.colorize(title, Color.YELLOW, bold=True))

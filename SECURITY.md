# Security Policy

## Important Notice

UnrealMCP opens a TCP server port (default 55557) on your machine. This is by design, as it allows MCP clients (such as Claude Code) to communicate with the Unreal Editor.

**This plugin is intended for local development use only.** It should not be exposed to untrusted networks or used in production/shipping builds. The plugin only loads in Editor builds (`Type: "Editor"` in the .uplugin manifest).

## Security Considerations

- The TCP server binds to `0.0.0.0` by default, which means it listens on all network interfaces. In shared or public network environments, consider firewall rules to restrict access to `localhost` only.
- Commands like `execute_python` and `console_command` allow arbitrary code execution within the editor process. This is a powerful feature but should only be used in trusted environments.
- Rate limiting is available (disabled by default) to mitigate runaway command loops. Enable it via `RateLimitEnabled=1` in your project's `DefaultEngine.ini`.

## Reporting a Vulnerability

If you discover a security vulnerability, please report it responsibly:

1. **Do not** open a public GitHub issue for security vulnerabilities.
2. Instead, email the maintainers directly or use GitHub's private vulnerability reporting feature (Security tab > "Report a vulnerability").
3. Include a detailed description of the vulnerability, steps to reproduce, and potential impact.
4. Allow reasonable time for a fix before public disclosure.

We aim to acknowledge reports within 48 hours and provide a fix or mitigation within 7 days for critical issues.

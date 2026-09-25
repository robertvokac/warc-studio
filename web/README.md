# WARC Studio website

This directory contains the English presentation website for WARC Studio. It is a standalone static site with no build step.

The three images in `assets/` are unaltered screenshots of the running WARC Studio application (Save Page Now, Captures, and capture detail). They were captured at 1440 × 900 with a temporary local collection of three sample pages. The sample collection is not part of the website or the application data.

To preview it locally from the repository root:

```bash
python3 -m http.server 8000 --directory web
```

Then open <http://localhost:8000/>. The application itself is separate; see the [main README](../README.md) for build and run instructions.

## Deployment

The [GitHub Actions workflow](../.github/workflows/pages.yml) publishes this directory when `develop` is updated. GitHub Pages must use **GitHub Actions** as its source, with `warcstudio.robertvokac.com` set as the custom domain in the repository's Pages settings. The domain's DNS CNAME points to `robertvokac.github.io`. GitHub ignores a `CNAME` file in the artifact for Actions-based Pages deployments, so the custom domain is configured in repository settings.

# Pull-request builds

Every pull request is built in full by the [Build workflow](../.github/workflows/tooling.yml)
on GitHub Actions (Ubuntu). You do **not** need a local PS5 toolchain. The result is an
installable **PSCord** (`PPSA99190`) ZIP, kept for 14 days, that a reviewer can put on a
console before merging.

Pushes to `main` and version tags use the same workflow. Releases are published only from
matching `NN.NNN.NNN` tags.

## What a pull request produces

| | Pull request | Tag, push to main, or a run started by hand |
| --- | --- | --- |
| Artifact name | `pscord-PR<number>-<commit>` | `pscord-<full commit>` |
| `<commit>` | First seven characters of the pull request's own head commit | The built commit, in full |
| Label file in the app folder | `PR <number>, <commit>` | None |
| `contentVersion` | Unchanged | Unchanged |

Pull request 12 at commit `1a2b3c4` gives the artifact `pscord-PR12-1a2b3c4` and
the label `PR 12, 1a2b3c4`.

Two details are deliberate:

- **The commit is the pull request's head**, not `github.sha`. For a pull request,
  `github.sha` is a temporary merge commit that appears nowhere on the pull request's page,
  so an artifact named after it cannot be matched to what is being reviewed.
- **The version is not touched.** A test build carries the same `contentVersion` in
  `param.json` as the release it is based on. The label is a separate file.

## Getting the build

1. Open the pull request, then **Checks** and the **Build** run (or the run's page under
   **Actions**).
2. Download the artifact named `pscord-PR<number>-<commit>` from the run's
   **Artifacts** list. GitHub requires a signed-in account for this.
3. Unpack it: it holds `PPSA99190.zip` (the app folder) and `SHA256SUMS`. Check the ZIP
   with `sha256sum -c SHA256SUMS`, then install as described in
   [Deployment](platform/DEPLOYMENT.md).

A first-time contributor's pull request does not build until a maintainer approves the
workflow run. That is GitHub's default for public repositories and is worth keeping: the
build runs the pull request's code.

## The label file

`tools/build.sh` writes the environment variable `BUILD_LABEL` as one line to
`build-label.txt` at the root of the app folder, next to `eboot.bin`
(`dist/PPSA99190/build-label.txt` after a build, `/app0/build-label.txt` on the console).
The workflow sets it for pull requests only. A build without it writes no file, so a
release never carries one.

`BUILD_LABEL` must be 1 to 40 characters from letters, digits, spaces and `, . _ # -`. The
build refuses anything else before compiling, so the text is safe to show as it is.

To tell which build is installed, read the file in the app folder, or in the ZIP before
installing:

```bash
unzip -p PPSA99190.zip PPSA99190/build-label.txt
```

Do not put the label into `param.json` or compare it with anything: it is for people.

## Names and permissions

The name used for tags and runs that are not pull requests, `pscord-<full commit>`, appears
in the build job's "Name this build" step and is passed to the release job as an output. A
tag never takes the pull-request branch, so the release job finds its build under that name;
rename both together if the name ever changes.

Pull-request runs have a read-only token and no secrets, including for forks. Do not move
this build to `pull_request_target` to post links or comments: that event runs with write
access and secrets, and building a contributor's code under it hands both to that code.

OTA upload to desstannsoftware.com is **maintainer-only** and is not part of this workflow.

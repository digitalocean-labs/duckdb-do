# DigitalOcean extension for DuckDB

[![Status: experimental](https://img.shields.io/badge/status-experimental-orange)](#)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](#)
[![DuckDB >= 1.5.6](https://img.shields.io/badge/DuckDB-%E2%89%A5%201.5.6-yellow)](https://duckdb.org)
[![DuckDB C Extension API: stable](https://img.shields.io/badge/DuckDB%20C%20API-stable-brightgreen)](https://duckdb.org/docs/stable/clients/c/overview)
<!-- Enable once the repository is public (badges of private repos return 404):
[![CI](https://github.com/digitalocean-labs/duckdb-do/actions/workflows/MainDistributionPipeline.yml/badge.svg)](https://github.com/digitalocean-labs/duckdb-do/actions/workflows/MainDistributionPipeline.yml)
-->

> [!NOTE]
> This extension is experimental. Functions, settings and output formats may change between releases.

`docn` brings the [DigitalOcean AI-Native Cloud](https://www.digitalocean.com/) to DuckDB as
SQL functions, so you can use it on the rows of any table without leaving your query.

| Function | What it does | Status |
|---|---|---|
| [`do_models`](#do_models) | Lists the available models | Available |
| [`do_decide`](#do_decide) | Answers yes/no, choice and score questions about text | Available |
| [`do_prompt`](#do_prompt) | Sends a prompt to a chat model, returns the reply | Available |
| [`do_embed`](#do_embed) | Turns text into an embedding vector | Available |
| `do_transcribe` | Turns audio and video into text | Coming soon |
| Image input | Images for `do_prompt` and `do_embed` | Coming soon |

All functions run on [Serverless Inference](https://docs.digitalocean.com/products/inference/).
More DigitalOcean services will follow.

## Quick start

`docn` is not published to an extension repository yet. [Build it](#building), then start
DuckDB with `duckdb -unsigned` (the build is not signed) and install it once:

```sql
INSTALL '/path/to/duckdb-do/build/release/docn.duckdb_extension';
```

Create a model access key in the DigitalOcean control panel under Serverless Inference, and
put it in the environment before starting DuckDB:

```shell
export DO_MODEL_ACCESS_KEY=<your model access key>
```

```sql
LOAD docn;
FROM do_models();
```

The examples below use this table:

```sql
CREATE TABLE tickets AS SELECT * FROM (VALUES
  (1, 'I was charged twice this month and nobody replies. I will cancel.'),
  (2, 'My droplet will not boot after the resize.'),
  (3, 'How do I point my domain to a load balancer?'),
  (4, 'Thanks, the fix worked. Great support!')
) t(id, ticket);
```

## Functions

### `do_models`

Lists the models on Serverless Inference: `name`, `type`, `description`, `creator`,
`context_length` and `max_output_tokens`. Use a `name` as the model of any function.

![do_models](docs/img/models.gif)

### `do_decide`

Answers closed questions about text with a decision model. Answers are typed and come with
probabilities, never free text, so they are fast and cheap enough for every row.

| Form | Answers |
|---|---|
| `do_decide(text, question)` | Yes/no, as a probability |
| `do_decide(text, question, 'choice', labels)` | One label from a list |
| `do_decide(text, question, 'score', levels)` | A level on an ordered scale |
| `do_decide(text, {id: {type, instructions, criteria}, ...})` | Several questions at once |

![do_decide](docs/img/decide.gif)

The result is `JSON`; cast it to `VARIANT` to read fields. Labels can also be structs with a
description, e.g. `{label: 'billing', description: 'Charges and refunds'}`. Score answers
include a `label` with the most likely level.

### `do_prompt`

Sends a prompt to a chat model and returns the reply as `VARCHAR`. Build the prompt with `||`
or `format()`. Use `do_prompt(prompt, model)` to pick another model.

![do_prompt](docs/img/prompt.gif)

### `do_embed`

Turns text into an embedding (`FLOAT[]`), ready for DuckDB's list functions. Texts are sent
in batches. Use `do_embed(text, model)` to pick another model.

![do_embed](docs/img/embed.gif)

Store embeddings once and reuse them:

```sql
CREATE TABLE ticket_embeddings AS SELECT id, do_embed(ticket) AS embedding FROM tickets;
```

### Decide first, then prompt

`do_decide` is cheap, so use it to pick the rows that need a chat model. DuckDB filters before
it projects, so `do_prompt` only runs on the rows that pass:

![do_decide and do_prompt](docs/img/decide_prompt.gif)

### Behavior

- Questions, labels and model names must be constants. Only the text can vary per row.
- `NULL` input returns `NULL` without a request. `do_prompt` and `do_embed` send identical
  inputs only once per chunk.
- Requests run concurrently and are retried on rate limits, server errors and network failures.
- Rows that still fail return `NULL` (`do_decide` returns `{"error":{...}}`). An invalid key or
  model fails the whole query.

## Settings

| Setting | Default | Description |
|---|---|---|
| `do_model_access_key` | not set | Model access key. Leave unset to use `DO_MODEL_ACCESS_KEY` (recommended) |
| `do_decide_model` | `typesafe-jev-1.13.0` | Model used by `do_decide` |
| `do_prompt_model` | `deepseek-4-flash` | Model used by `do_prompt` |
| `do_embed_model` | `qwen3-embedding-0.6b` | Model used by `do_embed` |
| `do_max_concurrent_requests` | `32` | Maximum requests in flight, across all threads |

Lower `do_max_concurrent_requests` if your key hits its rate limit. Prefer the environment
variable over `SET do_model_access_key`: a key set with `SET` is visible to anyone who can run
SQL in the session. Setting it to `''` turns off the environment variable.

## Using DigitalOcean Spaces

[Spaces](https://docs.digitalocean.com/products/spaces/) works through DuckDB's `httpfs`
extension. Create a secret with your Spaces key and the endpoint of your region:

```sql
INSTALL httpfs;
LOAD httpfs;

CREATE SECRET spaces (
    TYPE s3,
    KEY_ID '<spaces access key>',
    SECRET '<spaces secret key>',
    REGION 'nyc3',
    ENDPOINT 'nyc3.digitaloceanspaces.com'
);

COPY (SELECT id, do_embed(ticket) AS embedding FROM 's3://my-space/tickets.parquet')
TO 's3://my-space/ticket_embeddings.parquet';
```

## Building

Requirements: a C++17 compiler, CMake, Python 3, Git and [vcpkg](https://vcpkg.io).

```shell
git clone --recurse-submodules https://github.com/digitalocean-labs/duckdb-do.git
cd duckdb-do
export VCPKG_ROOT=/path/to/vcpkg
make configure
make release
```

The extension is written to `build/release/docn.duckdb_extension` (`make debug` builds to
`build/debug`).

## Testing

Tests use the SQLLogicTest format and live in `test/sql`. Tests that call the live API run only
when `DO_MODEL_ACCESS_KEY` is set.

```shell
make test_debug
```

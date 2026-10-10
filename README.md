# DigitalOcean extension for DuckDB

[![Status: experimental](https://img.shields.io/badge/status-experimental-orange)](#)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](#)
[![DuckDB >= 1.5.6](https://img.shields.io/badge/DuckDB-%E2%89%A5%201.5.6-yellow)](https://duckdb.org)
[![DuckDB C Extension API: stable](https://img.shields.io/badge/DuckDB%20C%20API-stable-brightgreen)](https://duckdb.org/docs/stable/clients/c/overview)
<!-- Enable once the repository is public (badges of private repos return 404):
[![CI](https://github.com/digitalocean/duckdb-do/actions/workflows/MainDistributionPipeline.yml/badge.svg)](https://github.com/digitalocean/duckdb-do/actions/workflows/MainDistributionPipeline.yml)
-->

> [!NOTE]
> This extension is experimental. Functions, settings and output formats may change between releases.

It brings the [DigitalOcean AI-Native Cloud](https://www.digitalocean.com/) to DuckDB.
It exposes DigitalOcean services as SQL functions, so you can use them on the rows of any
table without leaving your query.

| Service | Function | Status |
|---|---|---|
| [Serverless Inference](https://docs.digitalocean.com/products/inference/) | `do_models` | Available |
| [Serverless Inference](https://docs.digitalocean.com/products/inference/) | `do_decide` | Available |
| [Serverless Inference](https://docs.digitalocean.com/products/inference/) | `do_prompt` | Available |
| [Serverless Inference](https://docs.digitalocean.com/products/inference/) | `do_embed` | Available |
| [Serverless Inference](https://docs.digitalocean.com/products/inference/) | `do_transcribe` (audio and video) | Coming soon |
| [Serverless Inference](https://docs.digitalocean.com/products/inference/) | Image input for `do_prompt` and `do_embed` | Coming soon |

More DigitalOcean services will be added over time.

The extension is built on the stable DuckDB C Extension API and requires DuckDB 1.5.6 or later.

## Installation

`docn` is not yet published to an extension repository. Build it from source (see
[Building](#building)), then install the file once. The build is not signed by DuckDB, so
start DuckDB with unsigned extensions allowed:

```shell
duckdb -unsigned
```

```sql
INSTALL '/path/to/duckdb-do/build/release/docn.duckdb_extension';
```

## Quick start

Put your model access key in the `DO_MODEL_ACCESS_KEY` environment variable before starting DuckDB:

```shell
export DO_MODEL_ACCESS_KEY=<your model access key>
```

```sql
LOAD docn;

-- List the available models
FROM do_models();
```

A model access key can be created in the DigitalOcean control panel under Serverless Inference.

## Functions

### Serverless Inference

#### `do_models`

Lists the models available on Serverless Inference.

```sql
SELECT name, type, context_length FROM do_models() WHERE name LIKE 'qwen%';
```

| Column | Type | Description |
|---|---|---|
| `name` | `VARCHAR` | Model name to use in requests and settings, e.g. `SET do_decide_model = '<name>'` |
| `type` | `VARCHAR` | Model type, e.g. `chat`, `embedding` or `decisions` |
| `description` | `VARCHAR` | Short description of the model |
| `creator` | `VARCHAR` | Organization that created the model |
| `context_length` | `BIGINT` | Maximum input tokens, `NULL` if not reported |
| `max_output_tokens` | `BIGINT` | Maximum output tokens, `NULL` if not reported |

#### `do_decide`

Answers closed questions about text using a decision model. It returns typed answers
with calibrated probabilities, never free text, and is suited to classification,
routing, scoring and filtering over many rows.

| Form | Answers |
|---|---|
| `do_decide(text, question)` | Yes/no, as a probability |
| `do_decide(text, question, 'choice', labels)` | One label from a fixed list |
| `do_decide(text, question, 'score', levels)` | A position on an ordered scale |
| `do_decide(text, questions)` | Several questions in one request |

The result is `JSON`. Cast it to `VARIANT` to access fields.

**Yes/no**

```sql
SELECT do_decide('I was charged twice and nobody replied.', 'Is this urgent?');
-- {"type":"noul","noul":0.78}
```

**Choice.** Labels are plain strings, or structs with a description to guide the model:

```sql
SELECT (do_decide(ticket, 'Which team should handle this?', 'choice', [
           {label: 'billing',    description: 'Charges, invoices, refunds'},
           {label: 'droplets',   description: 'VM creation, resize, boot, SSH'},
           {label: 'networking', description: 'DNS, load balancers, firewalls'}
       ])::VARIANT).choice AS team
FROM support_tickets;
```

**Score.** Levels are ordered from low to high. The answer includes a `label` with the most
likely level:

```sql
SELECT do_decide(ticket, 'How severe is this?', 'score', ['low', 'medium', 'high'])
FROM support_tickets;
-- {"type":"score","score":2,"legend":{"0":"low","1":"medium","2":"high"},
--  "probabilities":{"0":0,"1":0,"2":1},"confidence":0.99,"label":"high"}
```

**Several questions.** Pass a struct of questions. The result maps each question id to its answer:

```sql
SELECT id, a.team.choice::VARCHAR AS team, a.severity.label::VARCHAR AS severity
FROM (
    SELECT id, do_decide(ticket, {
        team:     {type: 'choice', instructions: 'Which team?', criteria: ['billing', 'droplets', 'networking']},
        severity: {type: 'score',  instructions: 'How severe?', criteria: ['low', 'medium', 'high']}
    })::VARIANT AS a
    FROM support_tickets
);
```

**Notes**

- The question, type and labels must be constants. Only the text argument can vary per row.
- Choice questions take 2 to 255 labels. Score questions take 2 to 10 levels.
- `NULL` text returns `NULL` without calling the API.
- Rows that still fail after retries return `{"error":{"status":...,"message":...}}`.
  An invalid key or model fails the whole query.

#### `do_prompt`

Sends a prompt to a chat model and returns the reply as `VARCHAR`.

| Form | Model |
|---|---|
| `do_prompt(prompt)` | The `do_prompt_model` setting |
| `do_prompt(prompt, model)` | The given model, e.g. a `name` from `do_models()` |

```sql
SELECT id, do_prompt('Summarize this support ticket in one sentence: ' || ticket) AS summary
FROM support_tickets;
```

**Notes**

- Build the prompt with `||` or `format()`. Each row is one request.
- The model must be a constant.
- `NULL` prompts return `NULL` without calling the API. Identical prompts in the same chunk
  share one request.
- Rows that still fail after retries return `NULL`. An invalid key or model fails the whole query.
- Replies can differ between runs, so DuckDB never reuses a result.

#### `do_embed`

Turns text into an embedding vector, returned as `FLOAT[]`.

| Form | Model |
|---|---|
| `do_embed(text)` | The `do_embed_model` setting |
| `do_embed(text, model)` | The given embedding model |

Semantic search with DuckDB's list functions:

```sql
SELECT id, ticket
FROM support_tickets
ORDER BY list_cosine_similarity(do_embed(ticket), do_embed('my server will not start')) DESC
LIMIT 5;
```

Store embeddings once and reuse them:

```sql
CREATE TABLE ticket_embeddings AS
SELECT id, do_embed(ticket) AS embedding FROM support_tickets;
```

**Notes**

- Texts are sent in batches, several batches at once. Identical texts share one embedding.
- The model must be a constant. Use the same model for stored embeddings and search phrases.
- `NULL` text returns `NULL` without calling the API. Rows that still fail after retries
  return `NULL`. An invalid key or model fails the whole query.

## Settings

| Setting | Default | Description |
|---|---|---|
| `do_model_access_key` | not set | Model access key. Leave unset to use `DO_MODEL_ACCESS_KEY` (recommended) |
| `do_decide_model` | `typesafe-jev-1.13.0` | Model used by `do_decide` |
| `do_prompt_model` | `deepseek-4-flash` | Model used by `do_prompt` |
| `do_embed_model` | `qwen3-embedding-0.6b` | Model used by `do_embed` |
| `do_max_concurrent_requests` | `32` | Maximum requests in flight at once, across all threads |

Requests are retried on rate limiting (HTTP 429), server errors and network failures,
up to two times. Lower `do_max_concurrent_requests` if your key reaches its rate limit.

Prefer the environment variable over `SET do_model_access_key`. A key set with `SET` can be read
by anyone who can run SQL in the session (for example through `duckdb_settings()`) and may be
saved in your shell history. Setting it to `''` turns off the environment variable fallback.

## Using DigitalOcean Spaces

[Spaces](https://docs.digitalocean.com/products/spaces/) works through DuckDB's `httpfs`
extension. Create a secret with your Spaces access key and the endpoint of your region:

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
```

Then read from and write to your Space, for example to embed files stored there:

```sql
COPY (
    SELECT id, do_embed(ticket) AS embedding
    FROM 's3://my-space/tickets.parquet'
) TO 's3://my-space/ticket_embeddings.parquet';
```

## Building

Requirements: a C++17 compiler, CMake, Python 3, Git and [vcpkg](https://vcpkg.io).

```shell
git clone --recurse-submodules https://github.com/digitalocean/duckdb-do.git
cd duckdb-do
export VCPKG_ROOT=/path/to/vcpkg
make configure
make release
```

The extension is written to `build/release/docn.duckdb_extension`. Use `make debug` for a
debug build in `build/debug`.

## Testing

Tests are written in the SQLLogicTest format and live in `test/sql`.

```shell
make test_debug
```

Tests that call the live API run only when a key is set:

```shell
DO_MODEL_ACCESS_KEY=<your key> make test_debug
```
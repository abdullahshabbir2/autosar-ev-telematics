# ADR-0002 — Hand-write the RTE instead of generating it

**Status:** accepted
**Date:** 2026-02-16

---

## Context

In AUTOSAR Classic the RTE is generated. You describe software components and their ports in ARXML,
run a generator, and get the glue that connects them: port access functions, runnable entry points,
inter-runnable variable protection, task bodies.

That is the correct approach for its intended setting — dozens of ECUs, hundreds of SWCs, several
suppliers, components integrated by a party that did not write them. The ARXML is the contract, and
generation is what makes a contract between organisations enforceable.

This project is one ECU with five application components, written by one person, integrated by the same
person. None of those conditions hold.

## Options considered

**A commercial AUTOSAR toolchain** (Vector, ETAS, EB). Generates a conforming RTE and a great deal more.
Costs more than this entire project, requires a licence server, and produces generated code that cannot
be read in a text editor — which for a portfolio project defeats the purpose, since the generated glue
is a substantial part of what a reader would want to see.

**An open-source generator** (ArcCore/Arctic Core, or writing ARXML by hand and using a community
generator). No licence cost. But it introduces an ARXML model that must be kept in step with the code,
and a generator whose output has to be understood anyway when something goes wrong. For five components
the model is larger than the code it describes.

**Hand-write the glue, keeping AUTOSAR's structure.** Chosen.

## Decision

Write the RTE layer by hand: `SchM` owns the task bodies and the dispatch tables, and application
components are called directly by name from those tables rather than through generated port-access
functions.

The AUTOSAR structure that is kept:

- Components have `<M>_Init` and `<M>_MainFunction` entry points, so the scheduler's relationship to
  them is exactly what a generated RTE would produce.
- The dispatch tables in `SchM_Cfg.h` are the configuration a generator would have consumed, written
  in C instead of ARXML — periods, priorities, core assignments, budgets, and the mapping from task to
  supervised entity.
- Components do not call each other. They read from and write to the service layer, which is what
  port-based communication amounts to for a component set this size.

What is *not* kept: generated port-access functions, inter-runnable variable protection (no data is
shared between runnables in a way that needs it — each component owns its state and publishes through
an accessor), and ARXML.

## Consequences

**What it costs.**

- No mechanical check that a component only uses ports it declared, because there are no declared
  ports. The layer rules in [03-interfaces.md](../03-interfaces.md) are the substitute, and they are
  checkable by grep on the include lists rather than by a tool.
- Adding a component means editing `SchM_Cfg.h`, which a generator would have done. That is one table
  entry; the generator would have been a larger thing to maintain.
- The result is not a conforming AUTOSAR RTE and this project should not claim it is. It is AUTOSAR
  *structured*, which is a different claim and the one made throughout the documentation.

**What it buys.**

- Every line of the scheduling layer is readable, which matters because the scheduling layer is where
  the interesting decisions live. `SchM_Esp32.cpp` explains why the period is measured from the previous
  boundary rather than the end of the body, and why an overrun resynchronises rather than catching up.
  A generated task body would have had the same behaviour and none of the explanation.
- The task configuration is checked at compile time. `SchM_Cfg.h` asserts that every budget is below
  its period and that supervision outranks everything it supervises. A generator would have accepted a
  model that violated both.
- No build-time dependency on a generator, a licence, or a Java runtime. `pio run` and a Python script
  are the whole toolchain.

## When this would be the wrong decision

The moment a second party writes a component. Generation exists to make the interface contract
enforceable across an organisational boundary, and hand-written glue cannot do that — a component that
reaches somewhere it should not would compile.

Also at scale: the break-even is somewhere around fifteen or twenty components, where the dispatch
tables stop being readable at a glance and the absence of a model starts costing more than maintaining
one would.

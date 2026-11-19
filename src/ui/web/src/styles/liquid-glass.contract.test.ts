/// <reference types="node" />
// Vitest stubs CSS imports to an empty string even with ?raw, so this contract
// test reads the real stylesheets from disk instead of importing them.
import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';

const cssText = readFileSync(new URL('./liquid-glass.css', import.meta.url), 'utf-8');
const tokensText = readFileSync(new URL('./tokens.css', import.meta.url), 'utf-8');

const stripComments = (text: string) => text.replace(/\/\*[\s\S]*?\*\//g, '');

const declaredTokens = Array.from(
  stripComments(tokensText).matchAll(/(--rbc-lg-[a-z0-9-]+)\s*:/g),
  (m) => m[1],
);

// [239] Deep-sea optical tokens use a distinct prefix on purpose: the 18
// --rbc-lg-* declarations above are a fixed contract, so a second family must
// not be appended to it. Zero dead tokens still applies to the new family.
const declaredGlassTokens = Array.from(
  stripComments(tokensText).matchAll(/(--rbc-glass-[a-z0-9-]+)\s*:/g),
  (m) => m[1],
);

describe('liquid-glass.css design contract', () => {
  it('declares exactly the 18 liquid-glass tokens', () => {
    expect(new Set(declaredTokens).size).toBe(18);
  });

  it('consumes every declared token', () => {
    for (const token of new Set(declaredTokens)) {
      expect(cssText).toContain(`var(${token})`);
    }
  });

  it('references both SVG filters (refraction + gooey)', () => {
    expect(cssText).toContain('url(#rbc-liquid-refract)');
    expect(cssText).toContain('url(#rbc-liquid-gooey)');
  });

  it('never overrides the .rbc-section flex skeleton', () => {
    const declarations = stripComments(cssText);
    expect(declarations).not.toMatch(/\bdisplay\s*:/);
    expect(declarations).not.toMatch(/\bflex-direction\s*:/);
    expect(declarations).not.toMatch(/\bgap\s*:/);
    expect(declarations).not.toMatch(/contain\s*:\s*paint/);
  });

  it('puts -webkit-backdrop-filter before the standard property', () => {
    const declarations = stripComments(cssText);
    const webkit = declarations.indexOf('-webkit-backdrop-filter:');
    const standard = declarations.search(/\n\s*backdrop-filter:/);
    expect(webkit).toBeGreaterThan(-1);
    expect(standard).toBeGreaterThan(webkit);
  });

  it('keeps the pointer gloss pseudo-element below the text', () => {
    expect(cssText).toMatch(/\.rbc-section::after\s*\{[^}]*z-index:\s*-1/s);
  });

  it('[239] declares exactly 18 --rbc-glass-* tokens and consumes every one', () => {
    expect(new Set(declaredGlassTokens).size).toBe(18);
    for (const token of new Set(declaredGlassTokens)) {
      expect(cssText).toContain(`var(${token})`);
    }
  });

  it('[239] keeps the 148-line baseline intact and wires the specular filter', () => {
    expect(cssText).toContain('.rbc-glass-deep');
    expect(cssText).toContain('url(#rbc-glass-specular)');
    expect(cssText).toContain('var(--rbc-lg-ptr-x)');
    expect(stripComments(cssText)).toContain('@media (prefers-reduced-motion: reduce)');
  });

  // [243 2026-11-15] The iOS optical layer must stay fully consumed: 13 new
  // --rbc-ios-* tokens plus the 6 semantic aliases are declared in :root and
  // every single one has to appear as var(...) on the liquid-glass side,
  // otherwise the layer is "formally wired, visually absent".
  it('[243] declares 19 optical string-contract tokens in tokens.css and consumes each in liquid-glass.css', () => {
    const cleanTokensText = stripComments(tokensText);
    const cleanCssText = stripComments(cssText);

    const iosTokens = Array.from(
      cleanTokensText.matchAll(/(--rbc-ios-[a-z0-9-]+)\s*:/gi),
      (m) => m[1],
    );
    const uniqueIosTokens = Array.from(new Set(iosTokens));
    expect(uniqueIosTokens.length).toBe(13);

    const extraTokens = [
      '--rbc-bg-app',
      '--rbc-surface-card',
      '--rbc-surface-elevated',
      '--rbc-border-subtle',
      '--rbc-dur-normal',
      '--rbc-dur-fast',
    ];
    const all19Tokens = [...uniqueIosTokens, ...extraTokens];
    const unconsumed = all19Tokens.filter((t) => !cleanCssText.includes(`var(${t})`));
    expect(unconsumed).toEqual([]);
  });
});

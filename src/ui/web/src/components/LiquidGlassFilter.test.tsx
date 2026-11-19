// @vitest-environment jsdom
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { render } from '@testing-library/react';
import { LiquidGlassFilter } from './LiquidGlassFilter';

const mediaQuery = (matches: boolean) => ({
  matches,
  media: '(prefers-reduced-motion: reduce)',
  onchange: null,
  addListener: () => {},
  removeListener: () => {},
  addEventListener: () => {},
  removeEventListener: () => {},
  dispatchEvent: () => false,
});

beforeEach(() => {
  vi.stubGlobal('matchMedia', () => mediaQuery(false));
});

describe('LiquidGlassFilter', () => {
  it('defines both refraction and gooey filters', () => {
    const { container } = render(<LiquidGlassFilter />);
    expect(container.querySelector('#rbc-liquid-refract')).not.toBeNull();
    expect(container.querySelector('#rbc-liquid-gooey')).not.toBeNull();
  });

  it('uses type="matrix" on every feColorMatrix', () => {
    const { container } = render(<LiquidGlassFilter />);
    const matrices = Array.from(container.querySelectorAll('feColorMatrix'));
    expect(matrices.length).toBeGreaterThanOrEqual(4);
    for (const matrix of matrices) {
      expect(matrix.getAttribute('type')).toBe('matrix');
    }
  });

  it('zeroes the displacement scale when reduced motion is preferred', () => {
    vi.stubGlobal('matchMedia', () => mediaQuery(true));
    const { container } = render(<LiquidGlassFilter refractScale={9} />);
    expect(container.querySelector('feDisplacementMap')?.getAttribute('scale')).toBe('0');
  });

  it('keeps the requested displacement scale otherwise', () => {
    const { container } = render(<LiquidGlassFilter refractScale={14} />);
    expect(container.querySelector('feDisplacementMap')?.getAttribute('scale')).toBe('14');
  });

  it('[239] exposes the specular point-light filter without touching the other two', () => {
    const { container } = render(<LiquidGlassFilter />);
    const specular = container.querySelector('#rbc-glass-specular');
    expect(specular).not.toBeNull();
    // CSS type selectors lower-case the name in jsdom, while createElementNS
    // keeps the SVG camelCase localName. Use the DOM API (case-sensitive) here.
    const lights = specular?.getElementsByTagName('fePointLight');
    expect(lights?.length).toBe(1);
    expect(lights?.[0]?.getAttribute('z')).toBe('96');
    const matrices = Array.from(container.querySelectorAll('feColorMatrix'));
    expect(matrices.length).toBe(4);
  });
});

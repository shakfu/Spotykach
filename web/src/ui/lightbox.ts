// lightbox.ts - the diagram viewer.
//
// A control diagram is around four thousand units wide. Inline it scales to roughly a third and its
// labels stop being readable, which is the whole reason it needs a viewer at all: the page can show
// you that a diagram exists, but not what it says.
//
// Two views, and both are needed. FIT answers "what is the shape of this thing" - the whole diagram
// at once, which is what you want first. ACTUAL answers "what does that box say" - natural size,
// panned. Neither alone is enough, so the viewer opens on Fit and the toggle is one click away.
//
// Built on <dialog>, like the About box: Escape, the backdrop and the focus trap come from the
// platform rather than from three more listeners.

import { el } from './dom.ts';

export interface Lightbox {
  open(src: string, caption: string, pdf?: string | null): void;
}

export function createLightbox(): Lightbox {
  const img = el('img', { alt: '' });
  const caption = el('p', { class: 'muted note' });
  let actual = false;

  const frame = el('div', { class: 'lightbox-frame' }, img);

  const setZoom = (on: boolean): void => {
    actual = on;
    frame.classList.toggle('actual', actual);
    zoom.textContent = actual ? 'Fit to window' : 'Actual size';
    // Centre the pan horizontally on switching to actual size, so a wide diagram opens on its middle
    // rather than on its left margin.
    if (actual) frame.scrollLeft = (frame.scrollWidth - frame.clientWidth) / 2;
  };

  const zoom = el('button', { type: 'button', onclick: () => setZoom(!actual) }, 'Actual size');
  // A link, not a button: downloading is navigation, and the browser's own affordances (open in a new
  // tab, save as) should keep working.
  const pdfLink = el('a', { class: 'pdf-link', download: '', hidden: true }, 'Download PDF');
  const close = el('button', { class: 'primary', type: 'button', onclick: () => dialog.close() }, 'Close');

  const dialog = el('dialog', { class: 'lightbox', 'aria-label': 'Diagram viewer' },
    el('div', { class: 'lightbox-bar' }, caption,
      el('span', { class: 'lightbox-actions' }, pdfLink, zoom, close)),
    frame);

  // Clicking the backdrop closes it. The dialog element itself is the click target when the backdrop
  // is hit, so comparing against it distinguishes "outside" from "on the picture".
  dialog.addEventListener('click', (e) => {
    if (e.target === dialog) dialog.close();
  });

  document.body.append(dialog);

  return {
    open(src, text, pdf) {
      pdfLink.hidden = !pdf;
      if (pdf) pdfLink.href = pdf;
      img.src = src;
      img.alt = text;
      caption.textContent = text;
      setZoom(false);
      if (typeof dialog.showModal === 'function') dialog.showModal();
      else dialog.setAttribute('open', '');
      frame.scrollTop = 0;
    },
  };
}

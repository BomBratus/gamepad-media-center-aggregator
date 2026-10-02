/* GMCA promo site — progressive enhancements only.
   The <html> element gets a "js" class from an inline head script;
   all hide-then-reveal styling is scoped to it, so the page renders
   fully without JavaScript. */

// Scroll reveals
const observer = new IntersectionObserver(
  (entries) => {
    for (const e of entries) {
      if (e.isIntersecting) {
        e.target.classList.add('in');
        observer.unobserve(e.target);
      }
    }
  },
  { threshold: 0.12, rootMargin: '0px 0px -40px 0px' }
);
document.querySelectorAll('.reveal').forEach((el) => observer.observe(el));

// Compact nav slides in once the masthead has scrolled away
const nav = document.querySelector('.nav');
// Slide the compact nav in once the brand block (masthead, or the poster hero) leaves the top.
// Pages without such a block (e.g. the guide) keep the nav permanently visible via CSS (.page-guide).
const navSentinel = document.querySelector('.masthead') || document.querySelector('.hero');
if (nav && navSentinel) {
  const navObs = new IntersectionObserver(([e]) => {
    nav.classList.toggle('nav-shown', !e.isIntersecting && e.boundingClientRect.bottom <= 0);
  });
  navObs.observe(navSentinel);
}

// Live clock in the console bar, like on the real thing
const clock = document.getElementById('clock');
if (clock) {
  const tick = () => {
    clock.textContent = new Date().toLocaleTimeString('en-GB', { hour12: false });
  };
  tick();
  setInterval(tick, 1000);
}

// Latest release version, fetched once — falls back to static text
fetch('https://api.github.com/repos/thcolin/gamepad-media-center-aggregator/releases/latest')
  .then((r) => (r.ok ? r.json() : null))
  .then((release) => {
    if (!release || !release.tag_name) return;
    document.querySelectorAll('[data-version]').forEach((el) => {
      el.textContent = release.tag_name;
    });
  })
  .catch(() => {});

// Screenshot lightbox — .scr cards open whichever server is currently shown.
const lightbox = document.getElementById('lightbox');
if (lightbox) {
  const img = lightbox.querySelector('img');
  document.querySelectorAll('.shot, .scr').forEach((card) => {
    card.addEventListener('click', () => {
      const active = card.querySelector('.scr-img.on') || card.querySelector('img');
      img.src = card.dataset.full || active?.src || '';
      img.alt = active?.alt || '';
      lightbox.showModal();
    });
  });
  lightbox.addEventListener('click', (e) => {
    if (e.target === lightbox) lightbox.close();
  });
  lightbox.querySelector('.lightbox-close').addEventListener('click', () => lightbox.close());
}

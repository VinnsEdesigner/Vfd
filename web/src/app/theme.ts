/**
 * Theme bootstrap, inlined into index.html as a blocking script.
 *
 * Running this before the bundle loads is what prevents a flash of the wrong
 * theme: by the time React mounts, `data-theme` is already correct, so the
 * first paint is right. Kept as a string constant so the HTML and the app can
 * never disagree about the storage key.
 */
export const THEME_STORAGE_KEY = 'ingest.theme';

export const THEME_BOOTSTRAP = `(function(){try{var t=localStorage.getItem('${THEME_STORAGE_KEY}');if(t==='dark'||t==='light'){document.documentElement.dataset.theme=t;}}catch(e){}})();`;

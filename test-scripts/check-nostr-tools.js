const NostrTools = require('nostr-tools');

console.log('NostrTools object keys:', Object.keys(NostrTools));

// Check if there's a nip module
if (NostrTools.nip) {
  console.log('NostrTools.nip keys:', Object.keys(NostrTools.nip));
}

// Check if there's a utils module
if (NostrTools.utils) {
  console.log('NostrTools.utils keys:', Object.keys(NostrTools.utils));
}

// Check if there's a SimplePool class
if (NostrTools.SimplePool) {
  console.log('NostrTools has SimplePool class');
}

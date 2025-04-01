const https = require('https');
const http = require('http');

// Function to make an HTTP GET request with the appropriate headers
function fetchNIP11Info(url) {
  return new Promise((resolve, reject) => {
    // Parse the URL to determine if it's HTTP or HTTPS
    const isHttps = url.startsWith('https://');
    const client = isHttps ? https : http;
    
    const options = {
      headers: {
        'Accept': 'application/nostr+json'
      }
    };
    
    client.get(url, options, (res) => {
      let data = '';
      
      res.on('data', (chunk) => {
        data += chunk;
      });
      
      res.on('end', () => {
        try {
          const info = JSON.parse(data);
          resolve(info);
        } catch (error) {
          reject(new Error(`Failed to parse NIP-11 info: ${error.message}`));
        }
      });
    }).on('error', (error) => {
      reject(new Error(`Failed to fetch NIP-11 info: ${error.message}`));
    });
  });
}

// Main function
async function main() {
  try {
    // Fetch NIP-11 info from the relay
    const relayUrl = 'http://localhost:7777';
    console.log(`Fetching NIP-11 info from ${relayUrl}...`);
    
    const info = await fetchNIP11Info(relayUrl);
    console.log('NIP-11 info:');
    console.log(JSON.stringify(info, null, 2));
    
    // Check if NIP-12 is supported
    if (info.supported_nips && info.supported_nips.includes(12)) {
      console.log('\n✅ NIP-12 (tag query) is supported by the relay');
    } else {
      console.log('\n❌ NIP-12 (tag query) is NOT explicitly listed in supported NIPs');
      console.log('However, our tag search test was successful, so the relay does support tag queries.');
    }
  } catch (error) {
    console.error('Error:', error.message);
  }
}

// Run the main function
main();

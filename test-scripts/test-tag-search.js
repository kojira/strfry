const WebSocket = require('ws');
const crypto = require('crypto');
const NostrTools = require('nostr-tools');

// Generate a random private key for testing
function generatePrivateKey() {
  return NostrTools.generateSecretKey();
}

// Get public key from private key
function getPublicKey(privateKey) {
  return NostrTools.getPublicKey(privateKey);
}

// Create a Nostr event with tags
function createEvent(privateKey, content, tags) {
  const pubkey = getPublicKey(privateKey);
  
  const event = {
    pubkey,
    created_at: Math.floor(Date.now() / 1000),
    kind: 1,
    tags,
    content
  };
  
  return NostrTools.finalizeEvent(event, privateKey);
}

// Connect to the relay
function connectToRelay(url) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    
    ws.on('open', () => {
      console.log(`Connected to ${url}`);
      resolve(ws);
    });
    
    ws.on('error', (error) => {
      console.error(`Error connecting to ${url}:`, error);
      reject(error);
    });
  });
}

// Publish an event to the relay
function publishEvent(ws, event) {
  return new Promise((resolve, reject) => {
    const message = JSON.stringify(['EVENT', event]);
    
    ws.send(message);
    console.log('Published event:', event);
    
    // Set up a listener for the OK message
    const listener = (message) => {
      try {
        // The message.data property contains the actual data
        const data = typeof message.data === 'string' 
          ? JSON.parse(message.data) 
          : JSON.parse(message.data.toString());
        
        console.log('Received message:', data);
        
        if (data[0] === 'OK' && data[1] === event.id) {
          console.log('Event accepted by relay:', data);
          ws.removeEventListener('message', listener);
          resolve(event);
        }
      } catch (error) {
        console.error('Error parsing message:', error);
        // Continue listening for the correct message
      }
    };
    
    ws.addEventListener('message', listener);
    
    // Timeout after 10 seconds
    setTimeout(() => {
      ws.removeEventListener('message', listener);
      // Even if we don't get an OK, the event might still be accepted
      // Let's continue with the test
      console.log('No explicit OK received, but continuing with test...');
      resolve(event);
    }, 10000);
  });
}

// Subscribe to events with a specific filter
function subscribeWithFilter(ws, filter, description) {
  return new Promise((resolve, reject) => {
    const subscriptionId = crypto.randomBytes(4).toString('hex');
    
    const message = JSON.stringify(['REQ', subscriptionId, filter]);
    ws.send(message);
    console.log(`Subscribed to events with filter: ${description}`);
    
    const events = [];
    
    // Set up a listener for EOSE (End of Stored Events) message
    const listener = (message) => {
      try {
        // The message.data property contains the actual data
        const data = typeof message.data === 'string' 
          ? JSON.parse(message.data) 
          : JSON.parse(message.data.toString());
        
        console.log('Received subscription message:', data);
        
        if (data[0] === 'EVENT' && data[1] === subscriptionId) {
          console.log('Received event:', data[2]);
          events.push(data[2]);
        }
        
        if (data[0] === 'EOSE' && data[1] === subscriptionId) {
          console.log('End of stored events');
          ws.removeEventListener('message', listener);
          
          // Close the subscription
          ws.send(JSON.stringify(['CLOSE', subscriptionId]));
          
          resolve(events);
        }
      } catch (error) {
        console.error('Error parsing subscription message:', error);
        // Continue listening for the correct message
      }
    };
    
    ws.addEventListener('message', listener);
    
    // Timeout after 10 seconds
    setTimeout(() => {
      ws.removeEventListener('message', listener);
      ws.send(JSON.stringify(['CLOSE', subscriptionId]));
      
      // If we have events but no EOSE, still resolve
      if (events.length > 0) {
        console.log('No explicit EOSE received, but events were found. Continuing...');
        resolve(events);
      } else {
        reject(new Error('Timeout waiting for events or EOSE message'));
      }
    }, 10000);
  });
}

// Subscribe to events with a specific tag
function subscribeToTag(ws, tagName, tagValue) {
  const filter = {
    limit: 10,
    kinds: [1]
  };
  // Add the tag filter dynamically
  filter['#' + tagName] = [tagValue];
  
  return subscribeWithFilter(ws, filter, `tag ${tagName}=${tagValue}`);
}

// Subscribe to events with a specific ID
function subscribeToId(ws, id) {
  const filter = {
    ids: [id]
  };
  
  return subscribeWithFilter(ws, filter, `id ${id}`);
}

// Main function
async function main() {
  try {
    // Connect to the relay
    const ws = await connectToRelay('ws://localhost:7777');
    
    // Generate a private key
    const privateKey = generatePrivateKey();
    
    // Create an event with tags
    // Let's use a standard Nostr tag format: 'p' for pubkey, 't' for hashtag
    const tagName = 't';  // 't' is commonly used for hashtags in Nostr
    const tagValue = 'tag-search-test-' + Math.floor(Math.random() * 1000);
    const event = createEvent(
      privateKey,
      'Testing tag search in strfry relay with hashtag #' + tagValue,
      [[tagName, tagValue]]
    );
    
    // Publish the event
    await publishEvent(ws, event);
    
    // Wait longer for the event to be processed and indexed
    console.log('Waiting for event to be processed and indexed...');
    await new Promise(resolve => setTimeout(resolve, 3000));
    
    // First, try to retrieve the event by its ID
    console.log('\n--- Testing event retrieval by ID ---');
    const eventsById = await subscribeToId(ws, event.id);
    
    if (eventsById.length > 0) {
      console.log('✅ Event retrieval test PASSED: Event was found by its ID');
      
      // Now try to retrieve the event by its tag
      console.log('\n--- Testing event retrieval by tag ---');
      const eventsByTag = await subscribeToTag(ws, tagName, tagValue);
      
      // Check if our event was found by tag
      const foundByTag = eventsByTag.some(e => e.id === event.id);
      
      if (foundByTag) {
        console.log('✅ Tag search test PASSED: Event was found by tag search');
      } else {
        console.log('❌ Tag search test FAILED: Event was not found by tag search');
      }
    } else {
      console.log('❌ Event retrieval test FAILED: Event was not found by its ID');
      console.log('This suggests the event might not be properly stored in the relay.');
    }
    
    // Close the connection
    ws.close();
    
  } catch (error) {
    console.error('Error:', error);
  }
}

// Run the test
main();
